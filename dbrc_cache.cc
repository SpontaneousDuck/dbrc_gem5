#include "learning_gem5/mine/dbrc_cache.hh"

#include "base/random.hh"
#include "debug/DbrcCache.hh"
#include "sim/system.hh"

DbrcCache::DbrcCache(const DbrcCacheParams &params) :
    ClockedObject(params),
    latency(params.latency),
    blockSize(params.system->cacheLineSize()),
    capacity(params.size / blockSize),
    num_BTH(params.num_BTH),
    target_BTH(params.target_BTH),
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
    cache_L0T = new BTH_entry[params.system->memSize()/(L0T_offset)];
    cache_DBA = new DBA_entry[capacity];
}

DbrcCache::~DbrcCache()
{
    delete [] cache_L0T;
    delete [] cache_DBA;
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

uint32_t DbrcCache::GetLowestLevelIndex(Addr block_addr)
{
    uint32_t DBA_index = 0;
    uint32_t offset = blockSize/2;
    for (size_t i = 1; i < num_BTH; i++) {
        BTH_entry* entries = *(BTH_entry*)(cache_DBA[DBA_index].data);
        if(entries[block_addr/(L0T_offset/(offset))].valid)
            DBA_index = entries[(block_addr/(L0T_offset/(offset))) & (blockSize/2-1)].index;
            if(cache_DBA[DBA_index].dut.reutilization < 32)
                cache_DBA[DBA_index].dut.reutilization++;
        else
            return DBA_index;
        offset *= blockSize/2;
    }
}

/**
 * @brief Check if address exists in cache. Get/Set data if in cache.
 */
bool
DbrcCache::accessFunctional(PacketPtr pkt)
{
    uint32_t DBA_index = 0;
    Addr block_addr = pkt->getBlockAddr(blockSize);
    // TLB Lookup
    auto it = cache_TLB.find(block_addr/blockSize);
    if (it != cacheStore.end()) {
        DBA_index = it->second;
    }
    else
    {
        if(cache_L0T[block_addr/L0T_offset].valid)
            DBA_index = cache_L0T[block_addr/L0T_offset].index;
        else
            return false;

        

        if (cache_TLB.size > TLB_size)
        {
            it = cache_TLB.begin();
            cache_TLB.erase(it);
        }
        
        // TODO: implement storing BTH or data in TLB
        cache_TLB[block_addr/blockSize] = DBA_index;
    }

    if(cache_DBA[DBA_index].dut.valid && cache_DBA[DBA_index].dut.tag == block_addr/blockSize)
    {
        if (pkt->isWrite()) {
            // Write the data into the block in the cache
            pkt->writeDataToBlock(cache_DBA[DBA_index].data, blockSize);
        } else if (pkt->isRead()) {
            // Read the data out of the cache block into the packet
            pkt->setDataFromBlock(cache_DBA[DBA_index].data, blockSize);
        } else {
            panic("Unknown packet type!");
        }
        return true;
    }

    return false;
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
    // The packet should be aligned.
    assert(pkt->getAddr() ==  pkt->getBlockAddr(blockSize));
    // The address should not be in the TLB
    assert(cache_TLB.find(pkt->getAddr()) == cache_TLB.end());
    // The pkt should be a response
    assert(pkt->isResponse());

    size_t i;
    uint32_t smallest_r_idx = 0;
    // Select DBA vitim block
    while(i < MNA)
    {
        if!(!cache_DBA[VBIR].dut.lock)
        {
            if (!cache_DBA[VBIR].dut.valid ||
                !cache_DBA[VBIR].dut.parent_valid ||
                cache_DBA[VBIR].dut.reutilization == 0)
            {
                break;
            }
            else
            {
                if(smallest_r_idx == 0 || cache_DBA[VBIR].dut.reutilization < smallest_r_idx)
                    smallest_r_idx = VBIR;
                cache_DBA[VBIR].dut.reutilization = 0;
            }
        }
        
        VBIR++;
        if(VBIR>capacity)
            VBIR=0;
        MNA++;
    }

    // Select smallest R value if no suitable found in MAximum NUmber of Attempts
    if(i == MNA)
        VBIR = smallest_r_idx;
    


    if (cacheStore.size() >= capacity) {
        // Select random thing to evict. This is a little convoluted since we
        // are using a std::unordered_map. See http://bit.ly/2hrnLP2
        int bucket, bucket_size;
        do {
            bucket = random_mt.random(0, (int)cacheStore.bucket_count() - 1);
        } while ( (bucket_size = cacheStore.bucket_size(bucket)) == 0 );
        auto block = std::next(cacheStore.begin(bucket),
                               random_mt.random(0, bucket_size - 1));

        DPRINTF(DbrcCache, "Removing addr %#x\n", block->first);

        // Write back the data.
        // Create a new request-packet pair
        RequestPtr req = std::make_shared<Request>(
            block->first, blockSize, 0, 0);

        PacketPtr new_pkt = new Packet(req, MemCmd::WritebackDirty, blockSize);
        new_pkt->dataDynamic(block->second); // This will be deleted later

        DPRINTF(DbrcCache, "Writing packet back %s\n", pkt->print());
        // Send the write to memory
        memPort.sendPacket(new_pkt);

        // Delete this entry
        cacheStore.erase(block->first);
    }

    DPRINTF(DbrcCache, "Inserting %s\n", pkt->print());
    DDUMP(DbrcCache, pkt->getConstPtr<uint8_t>(), blockSize);

    // Allocate space for the cache block data
    uint8_t *data = new uint8_t[blockSize];

    // Insert the data and address into the cache store
    cacheStore[pkt->getAddr()] = data;

    // Write the data into the cache
    pkt->writeDataToBlock(data, blockSize);
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