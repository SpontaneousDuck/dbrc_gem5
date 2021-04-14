#include "learning_gem5/mine/dbrc_cache.hh"

#include "base/random.hh"
#include "debug/DbrcCache.hh"
#include "sim/system.hh"

DbrcCache::DbrcCache(const DbrcCacheParams &params) :
    ClockedObject(params),
    latency(params.latency),
    blockSize(params.system->cacheLineSize()),
    capacity(params.size / blockSize),
    target_BTH(params.target_BTH),
    num_BTH(params.num_BTH),
    TLB_size(params.TLB_size),
    MNA(params.MNA),
    memPort(params.name + ".mem_side", this),
    blocked(false), originalPacket(nullptr), waitingPortId(-1), stats(this)
{
    // Since the CPU side ports are a vector of ports, create an instance of
    // the CPUSidePort for each connection. This member of params is
    // automatically created depending on the name of the vector port and
    // holds the number of connections to this port name
    for (int i = 0; i < params.port_cpu_side_connection_count; ++i) {
        cpuPorts.emplace_back(name() + csprintf(".cpu_side[%d]", i), i, this);
    }

    VBIR = 0;

    L0T_offset = blockSize;
    for(size_t i = 1; i < num_BTH; i++)
        L0T_offset *= (blockSize/2);
    cache_L0T = (BTH_entry*)calloc((1UL<<32)/(L0T_offset), sizeof(BTH_entry));
    cache_DBA = (DBA_entry*)calloc(capacity, sizeof(DBA_entry));

    for (size_t i = 0; i < capacity; i++)
    {
        cache_DBA[i].BTH = (BTH_entry*)calloc(blockSize/2, sizeof(BTH_entry));
        cache_DBA[i].data = (uint8_t*)calloc(blockSize, sizeof(uint8_t));
    }
}

DbrcCache::~DbrcCache()
{
    for (size_t i = 0; i < capacity; i++)
    {
        free(cache_DBA[i].data);
    }
    
    free(cache_L0T);
    free(cache_DBA);
}

Port &
DbrcCache::getPort(const std::string &if_name, PortID idx)
{
    // This is the name from the Python SimObject declaration in DbrcCache.py
    if (if_name == "mem_side") {
        panic_if(idx != InvalidPortID,
                 "Mem side of simple cache not a vector port");
        return memPort;
    } else if (if_name == "cpu_side" && idx < cpuPorts.size()) {
        // We should have already created all of the ports in the constructor
        return cpuPorts[idx];
    } else {
        // pass it along to our super class
        return ClockedObject::getPort(if_name, idx);
    }
}

void
DbrcCache::CPUSidePort::sendPacket(PacketPtr pkt)
{
    // Note: This flow control is very simple since the cache is blocking.

    panic_if(blockedPacket != nullptr, "Should never try to send if blocked!");

    // If we can't send the packet across the port, store it for later.
    DPRINTF(DbrcCache, "Sending %s to CPU\n", pkt->print());
    if (!sendTimingResp(pkt)) {
        DPRINTF(DbrcCache, "failed!\n");
        blockedPacket = pkt;
    }
}

AddrRangeList
DbrcCache::CPUSidePort::getAddrRanges() const
{
    return owner->getAddrRanges();
}

void
DbrcCache::CPUSidePort::trySendRetry()
{
    if (needRetry && blockedPacket == nullptr) {
        // Only send a retry if the port is now completely free
        needRetry = false;
        DPRINTF(DbrcCache, "Sending retry req.\n");
        sendRetryReq();
    }
}

void
DbrcCache::CPUSidePort::recvFunctional(PacketPtr pkt)
{
    // Just forward to the cache.
    return owner->handleFunctional(pkt);
}

bool
DbrcCache::CPUSidePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(DbrcCache, "Got request %s\n", pkt->print());

    if (blockedPacket || needRetry) {
        // The cache may not be able to send a reply if this is blocked
        DPRINTF(DbrcCache, "Request blocked\n");
        needRetry = true;
        return false;
    }
    // Just forward to the cache.
    if (!owner->handleRequest(pkt, id)) {
        DPRINTF(DbrcCache, "Request failed\n");
        // stalling
        needRetry = true;
        return false;
    } else {
        DPRINTF(DbrcCache, "Request succeeded\n");
        return true;
    }
}

void
DbrcCache::CPUSidePort::recvRespRetry()
{
    // We should have a blocked packet if this function is called.
    assert(blockedPacket != nullptr);

    // Grab the blocked packet.
    PacketPtr pkt = blockedPacket;
    blockedPacket = nullptr;

    DPRINTF(DbrcCache, "Retrying response pkt %s\n", pkt->print());
    // Try to resend it. It's possible that it fails again.
    sendPacket(pkt);

    // We may now be able to accept new packets
    trySendRetry();
}

void
DbrcCache::MemSidePort::sendPacket(PacketPtr pkt)
{
    // Note: This flow control is very simple since the cache is blocking.

    panic_if(blockedPacket != nullptr, "Should never try to send if blocked!");

    // If we can't send the packet across the port, store it for later.
    if (!sendTimingReq(pkt)) {
        blockedPacket = pkt;
    }
}

bool
DbrcCache::MemSidePort::recvTimingResp(PacketPtr pkt)
{
    // Just forward to the cache.
    return owner->handleResponse(pkt);
}

void
DbrcCache::MemSidePort::recvReqRetry()
{
    // We should have a blocked packet if this function is called.
    assert(blockedPacket != nullptr);

    // Grab the blocked packet.
    PacketPtr pkt = blockedPacket;
    blockedPacket = nullptr;

    // Try to resend it. It's possible that it fails again.
    sendPacket(pkt);
}

void
DbrcCache::MemSidePort::recvRangeChange()
{
    owner->sendRangeChange();
}

/**
 * @brief Handle requests for a blocking cache. Delay by cache latency.
 */
bool
DbrcCache::handleRequest(PacketPtr pkt, int port_id)
{
    if (blocked) {
        // There is currently an outstanding request so we can't respond. Stall
        return false;
    }

    DPRINTF(DbrcCache, "Got request for addr %#x\n", pkt->getAddr());

    // This cache is now blocked waiting for the response to this packet.
    blocked = true;

    // Store the port for when we get the response
    assert(waitingPortId == -1);
    waitingPortId = port_id;

    // Schedule an event after cache access latency to actually access
    schedule(new EventFunctionWrapper([this, pkt]{ accessTiming(pkt); },
                                      name() + ".accessEvent", true),
             clockEdge(latency));

    return true;
}

bool
DbrcCache::handleResponse(PacketPtr pkt)
{
    assert(blocked);
    DPRINTF(DbrcCache, "Got response for addr %#x\n", pkt->getAddr());

    // For now assume that inserts are off of the critical path and don't count
    // for any added latency.
    insert(pkt);

    stats.missLatency.sample(curTick() - missTime);

    // If we had to upgrade the request packet to a full cache line, now we
    // can use that packet to construct the response.
    if (originalPacket != nullptr) {
        DPRINTF(DbrcCache, "Copying data from new packet to old\n");
        // We had to upgrade a previous packet. We can functionally deal with
        // the cache access now. It better be a hit.
        M5_VAR_USED bool hit = accessFunctional(originalPacket);
        panic_if(!hit, "Should always hit after inserting");
        originalPacket->makeResponse();
        delete pkt; // We may need to delay this, I'm not sure.
        pkt = originalPacket;
        originalPacket = nullptr;
    } // else, pkt contains the data it needs

    sendResponse(pkt);

    return true;
}

void DbrcCache::sendResponse(PacketPtr pkt)
{
    assert(blocked);
    DPRINTF(DbrcCache, "Sending resp for addr %#x\n", pkt->getAddr());

    int port = waitingPortId;

    // The packet is now done. We're about to put it in the port, no need for
    // this object to continue to stall.
    // We need to free the resource before sending the packet in case the CPU
    // tries to send another request immediately (e.g., in the same callchain).
    blocked = false;
    waitingPortId = -1;

    // Simply forward to the memory port
    cpuPorts[port].sendPacket(pkt);

    // For each of the cpu ports, if it needs to send a retry, it should do it
    // now since this memory object may be unblocked now.
    for (auto& port : cpuPorts) {
        port.trySendRetry();
    }
}

/**
 * @brief Functional implentation of cache. Respond if hit, forward if miss.
 */
void
DbrcCache::handleFunctional(PacketPtr pkt)
{
    if (accessFunctional(pkt)) {
        pkt->makeResponse();
    } else {
        memPort.sendFunctional(pkt);
    }
}

/**
 * @brief Craete response packet if hit. Format cache line request and forward if miss.
 */
void
DbrcCache::accessTiming(PacketPtr pkt)
{
    bool hit = accessFunctional(pkt);

    DPRINTF(DbrcCache, "%s for packet: %s\n", hit ? "Hit" : "Miss",
            pkt->print());

    if (hit) {
        // Respond to the CPU side
        stats.hits++; // update stats
        DDUMP(DbrcCache, pkt->getConstPtr<uint8_t>(), pkt->getSize());
        pkt->makeResponse();
        sendResponse(pkt);
    } else {
        stats.misses++; // update stats
        missTime = curTick();
        // Forward to the memory side.
        // We can't directly forward the packet unless it is exactly the size
        // of the cache line, and aligned. Check for that here.
        Addr addr = pkt->getAddr();
        Addr block_addr = pkt->getBlockAddr(blockSize);
        unsigned size = pkt->getSize();
        if (addr == block_addr && size == blockSize) {
            // Aligned and block size. We can just forward.
            DPRINTF(DbrcCache, "forwarding packet\n");
            memPort.sendPacket(pkt);
        } else {
            DPRINTF(DbrcCache, "Upgrading packet to block size\n");
            panic_if(addr - block_addr + size > blockSize,
                     "Cannot handle accesses that span multiple cache lines");
            // Unaligned access to one cache block
            assert(pkt->needsResponse());
            MemCmd cmd;
            if (pkt->isWrite() || pkt->isRead()) {
                // Read the data from memory to write into the block.
                // We'll write the data in the cache (i.e., a writeback cache)
                cmd = MemCmd::ReadReq;
            } else {
                panic("Unknown packet type in upgrade size");
            }

            // Create a new packet that is blockSize
            PacketPtr new_pkt = new Packet(pkt->req, cmd, blockSize);
            new_pkt->allocate();

            // Should now be block aligned
            assert(new_pkt->getAddr() == new_pkt->getBlockAddr(blockSize));

            // Save the old packet
            originalPacket = pkt;

            DPRINTF(DbrcCache, "forwarding packet\n");
            memPort.sendPacket(new_pkt);
        }
    }
}

// Search DBRC for data block
bool DbrcCache::CacheSearch(Addr block_addr, uint32_t &index)
{
    uint32_t offset = blockSize/2;
    
    // L0T Search
    if(cache_L0T[block_addr/L0T_offset].V)
        index = cache_L0T[block_addr/L0T_offset].I;
    else
    {
        index = -1;
        return false;
    }

    // LNT Search
    for (size_t i = 1; i < num_BTH; i++) 
    {
        BTH_entry* entries = cache_DBA[index].BTH;
        uint32_t idx = (block_addr/(L0T_offset/(offset))) & (blockSize/2-1);
        if(entries[idx].V)
        {
            index = entries[idx].I;
            if(cache_DBA[index].dut.R < 32)
                cache_DBA[index].dut.R++;
        }
        else
            return false;
        offset *= blockSize/2;
    }

    // Validate data DUT entry
    if(cache_DBA[index].dut.LF != num_BTH || !cache_DBA[index].dut.V || cache_DBA[index].tt.TAG != block_addr/blockSize)
    {
        return false;
    }

    return true;
}

/**
 * @brief Check if address exists in cache. Get/Set data if in cache.
 */
bool
DbrcCache::accessFunctional(PacketPtr pkt)
{
    uint32_t DBA_index = 0;
    Addr block_addr = pkt->getBlockAddr(blockSize);
    
    // TLB Search
    auto it = cache_TLB.find(block_addr/blockSize);
    if (it != cache_TLB.end()) {
        DBA_index = it->second;
        auto it2 = std::find(cache_TLB_order.begin(), cache_TLB_order.end(), (uint32_t)(block_addr/blockSize));
        assert(it2 != cache_TLB_order.end());
        cache_TLB_order.erase(it2);
        cache_TLB_order.push_back((uint32_t)(block_addr/blockSize));
    }
    // Full Cache Search
    else if(CacheSearch(block_addr, DBA_index))
    {
        // Write cache find to TLB
        if (cache_TLB.size() > TLB_size)
        {
            cache_TLB.erase(cache_TLB_order.front());
            cache_TLB_order.erase(cache_TLB_order.begin());
        }
        
        // TODO: implement storing BTH or data in TLB
        cache_TLB[block_addr/blockSize] = DBA_index;
        cache_TLB_order.push_back(block_addr/blockSize);
    } else
    {
        return false;
    }

    // Perform Operation on found cache block
    if (pkt->isWrite()) {
        // Write the data into the block in the cache
        pkt->writeDataToBlock(cache_DBA[DBA_index].data, blockSize);
        cache_DBA[DBA_index].dut.D = true;
    } else if (pkt->isRead()) {
        // Read the data out of the cache block into the packet
        pkt->setDataFromBlock(cache_DBA[DBA_index].data, blockSize);
    } else {
        panic("Unknown packet type!");
    }

    return true;
}

/**
 * @brief Insert data in to cache after memory response. Handle write-back and replacement policy.
 * 
 * @details 
 *      1.  b = Select a DBA victim block
 *      2.  Make the BTH entry in level N point to b
 *      3.  if (b's DUT entry bits V==true and PV==true)
 *      3.1     Invalidate the entry of the BTH table that points to b
 *      3.2     Invalidate tan eventual entry in the B-TLB that points to b
 *      3.3     if (b'2 DUT entry LF field indicates the b holds a BTH table)
 *      3.3.1       Invalidate DUT entries associated with b's children
 *      3.4     else if (b's DUT entry dirty bit D==true)
 *      3.4.1       Save b's contents into physical memory
 *      4.  Install block level N+1
 *      5.  if (++N < data block level) goto 1
 */
void
DbrcCache::insert(PacketPtr pkt)
{
    uint32_t last_BTH, current_level;
    Addr address = pkt->getAddr();

    // The packet should be aligned.
    assert(address ==  pkt->getBlockAddr(blockSize));
    // Address should not be valid in the Cache. Set last valid BTH index.
    assert(!CacheSearch(address, last_BTH));
    // The address should not be in the TLB
    assert(cache_TLB.find(address/blockSize) == cache_TLB.end());
    // The pkt should be a response
    assert(pkt->isResponse());


    // Miss in L0T
    if (last_BTH == -1)
    {
        // Invalidate DUT entries associated with b's children
        if(cache_L0T[address/L0T_offset].V)
        {
            cache_DBA[cache_L0T[address/L0T_offset].I].dut.PV = false;
        }

        current_level = 0;
    }
    else
    {
        current_level = cache_DBA[last_BTH].dut.LF;
    }

    current_level++;

    while(current_level <= num_BTH)
    {
        size_t i = 0;
        uint32_t smallest_r_idx = -1;
        uint32_t smallest_r = 33;
        // Select DBA vitim block
        while(i < MNA)
        {
            if(!cache_DBA[VBIR].dut.L)
            {
                if (!cache_DBA[VBIR].dut.V ||
                    !cache_DBA[VBIR].dut.PV ||
                    cache_DBA[VBIR].dut.R == 0)
                {
                    break;
                }
                else
                {
                    if(cache_DBA[VBIR].dut.R < smallest_r)
                    {
                        smallest_r_idx = VBIR;
                        smallest_r = cache_DBA[VBIR].dut.R;
                    }
                    cache_DBA[VBIR].dut.R = 0;
                }
                i++;
            }
            
            VBIR++;
            if(VBIR>=capacity)
                VBIR=0;
        }

        // If b was valid
        if(cache_DBA[VBIR].dut.V == true && cache_DBA[VBIR].dut.LF > 0)
        {
            if(cache_DBA[VBIR].dut.PV == true)
            {
                // Invalidate the entry of the BTH table that points to b
                if(cache_DBA[VBIR].dut.LF == 1)
                    cache_L0T[cache_DBA[VBIR].tt.PT].V = false;
                else
                {
                    for (i = 0; i < blockSize/2; i++)
                    {
                        if (cache_DBA[cache_DBA[VBIR].tt.PT].BTH[i].I == VBIR)
                        {
                            cache_DBA[cache_DBA[VBIR].tt.PT].BTH[i].V = false;
                            break;
                        }
                    }
                }
            }

            // If is data, invalidate an entry in the B-TLB that points to b
            //TODO: BTH in TLB
            if (cache_DBA[VBIR].dut.LF == num_BTH)
            {
                auto it = cache_TLB.find(cache_DBA[VBIR].tt.TAG);
                if (it != cache_TLB.end()) {
                    cache_TLB.erase(it);
                    auto it2 = std::find(cache_TLB_order.begin(), cache_TLB_order.end(), (uint32_t)(cache_DBA[VBIR].tt.TAG));
                    assert(it2 != cache_TLB_order.end());
                    cache_TLB_order.erase(it2);
                }
                cache_DBA[VBIR].tt.TAG = 0;
            }

            // if (b's DUT entry LF field indicates the b holds a BTH table)
            if(cache_DBA[VBIR].dut.LF < num_BTH)
            {
                for (i = 0; i < blockSize/2; i++)
                {
                    // Invalidate DUT entries associated with b's children
                    if(((BTH_entry*)(cache_DBA[VBIR].BTH))[i].V)
                    {
                        cache_DBA[((BTH_entry*)(cache_DBA[VBIR].BTH))[i].I].dut.PV = false;
                    }
                }
            }
            // else if (b's DUT entry dirty bit D==true)
            else if(cache_DBA[VBIR].dut.D)
            {
                // Save b's contents into physical memory
                // Create a new request-packet pair
                RequestPtr req = std::make_shared<Request>(
                    cache_DBA[VBIR].tt.TAG, blockSize, 0, 0);

                PacketPtr new_pkt = new Packet(req, MemCmd::WritebackDirty, blockSize);
                new_pkt->dataDynamic(cache_DBA[VBIR].data); // This will be deleted later

                DPRINTF(DbrcCache, "Writing packet back %s\n", pkt->print());
                // Send the write to memory
                memPort.sendPacket(new_pkt);
            }
        }

        // Select smallest R value if no suitable found in Maximum Number of Attempts
        if(i == MNA)
            VBIR = smallest_r_idx;
        
        if (current_level == 1)
        {
            // Make the BTH entry in L0T point to b and set valid
            cache_L0T[address/L0T_offset].I = VBIR;
            cache_L0T[address/L0T_offset].V = true;
        }
        else
        {
            // Make the BTH entry in level N point to b and set valid
            cache_DBA[last_BTH].BTH[(address/(uint32_t)(L0T_offset/pow(blockSize/2, current_level-1)))&(blockSize/2-1)].I = VBIR;
            cache_DBA[last_BTH].BTH[(address/(uint32_t)(L0T_offset/pow(blockSize/2, current_level-1)))&(blockSize/2-1)].V = true;
        }    

        // Install block level N+1
        // Clear data memory
        std::memset(cache_DBA[VBIR].data, 0, blockSize*sizeof(uint8_t));
        std::memset(cache_DBA[VBIR].BTH, 0, blockSize/2*sizeof(BTH_entry));
        
        cache_DBA[VBIR].dut.V = true;
        cache_DBA[VBIR].dut.PV = true;
        cache_DBA[VBIR].dut.LF = current_level;
        cache_DBA[VBIR].dut.R = 1;
        if (current_level == 1)
            cache_DBA[VBIR].tt.PT = address/L0T_offset;
        else
            cache_DBA[VBIR].tt.PT = last_BTH;

        last_BTH = VBIR;
        current_level++;
        VBIR++;
        if(VBIR>=capacity)
        {
            VBIR=0;
        }

        // if (++N < data block level) goto 1
    }

    // DPRINTF(DbrcCache, "Inserting %s\n", pkt->print());
    // DDUMP(DbrcCache, pkt->getConstPtr<uint8_t>(), blockSize);


    DPRINTF(DbrcCache, "Inserting %s\n", pkt->print());
    DDUMP(DbrcCache, pkt->getConstPtr<uint8_t>(), blockSize);

    cache_DBA[last_BTH].tt.TAG = address/blockSize;

    // Write cache find to TLB
    if (cache_TLB.size() > TLB_size)
    {
        cache_TLB.erase(cache_TLB_order.front());
        cache_TLB_order.erase(cache_TLB_order.begin());
    }

    cache_TLB[address/blockSize] = last_BTH;
    cache_TLB_order.push_back(address/blockSize);

    // Write the data into the cache
    pkt->writeDataToBlock(cache_DBA[last_BTH].data, blockSize);
}

AddrRangeList
DbrcCache::getAddrRanges() const
{
    DPRINTF(DbrcCache, "Sending new ranges\n");
    // Just use the same ranges as whatever is on the memory side.
    return memPort.getAddrRanges();
}

void
DbrcCache::sendRangeChange() const
{
    for (auto& port : cpuPorts) {
        port.sendRangeChange();
    }
}

DbrcCache::DbrcCacheStats::DbrcCacheStats(Stats::Group *parent)
      : Stats::Group(parent),
      ADD_STAT(hits, UNIT_COUNT, "Number of hits"),
      ADD_STAT(misses, UNIT_COUNT, "Number of misses"),
      ADD_STAT(missLatency, UNIT_TICK, "Ticks for misses to the cache"),
      ADD_STAT(hitRatio, UNIT_RATIO,
               "The ratio of hits to the total accesses to the cache",
               hits / (hits + misses))
{
    missLatency.init(16); // number of buckets
}