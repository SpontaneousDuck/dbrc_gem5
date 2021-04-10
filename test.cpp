#include <cstdint>
#include <cstdio>
#include <cassert>
#include <map>

typedef uint64_t Addr;

typedef struct BTH_entry
{
  bool V = 0; //valid
  uint32_t I = 0; //index
} BTH_entry;

typedef struct DUT_entry
{
  bool V = 0; //valid
  bool D = 0; //dirty
  bool L = 0; //lock
  uint8_t LF = 0; //level
  bool PV = false; //parent_valid
  uint8_t R = 0; //reutilization
} DUT_entry;

typedef struct TT_entry
{
  uint32_t TAG = 0;
  uint32_t PT = 0; //parent_table
} TT_entry;

typedef struct DBA_entry
{
  uint8_t* data;
  DUT_entry dut;
  TT_entry tt;
} DBA_entry;

/// The block size for the cache
const unsigned blockSize = 64;
/// Number of blocks in the cache (size of cache / block size)
const unsigned capacity = (1<<20)/blockSize;
const unsigned target_BTH = 3;
const unsigned num_BTH = 3;
const unsigned TLB_size = (1<<16);
const unsigned MNA = 5;
uint32_t L0T_offset; 

/// TLB buffer. Unordered map since fully-associative
std::map<uint32_t, uint32_t> cache_TLB;
uint32_t VBIR; 
BTH_entry* cache_L0T;
DBA_entry* cache_DBA;

bool CacheSearch(Addr block_addr, uint32_t &index)
{
    uint32_t offset = blockSize/2;
    
    // L0T Search
    if(cache_L0T[block_addr/L0T_offset].V)
        index = cache_L0T[block_addr/L0T_offset].I;
    else
        index = -1;
        return false;

    // LNT Search
    for (size_t i = 1; i < num_BTH; i++) {
        BTH_entry* entries = (BTH_entry*)(cache_DBA[index].data);
        if(entries[block_addr/(L0T_offset/(offset))].V)
        {
            uint32_t idx = (block_addr/(L0T_offset/(offset))) & (blockSize/2-1);
            index = entries[idx].I;
            if(cache_DBA[index].dut.R < 32)
                cache_DBA[index].dut.R++;
        }
        else
            return false;
        offset *= blockSize/2;
    }

    // Validate data DUT entry
    if(cache_DBA[index].dut.LF != num_BTH || !cache_DBA[index].dut.V && cache_DBA[index].tt.TAG != block_addr/blockSize)
    {
        return false;
    }

    return true;
}

/**
 * @brief Check if address exists in cache. Get/Set data if in cache.
 */
bool accessFunctional(Addr block_addr, uint8_t *data, bool isWrite)
// bool accessFunctional(PacketPtr pkt)
{
    uint32_t DBA_index = 0;
    // Addr block_addr = pkt->getBlockAddr(blockSize);
    
    // TLB Search
    auto it = cache_TLB.find(block_addr/blockSize);
    if (it != cache_TLB.end()) {
        DBA_index = it->second;
    }
    // Full Cache Search
    else if(CacheSearch(block_addr, DBA_index))
    {
        // Write cache find to TLB
        if (cache_TLB.size() > TLB_size)
        {
            auto it = cache_TLB.begin();
            cache_TLB.erase(it);
        }
        
        // TODO: implement storing BTH or data in TLB
        cache_TLB[block_addr/blockSize] = DBA_index;
    } else
    {
        return false;
    }
    
    // Perform Operation on found cache block
    if (isWrite) {
        // Write the data into the block in the cache
        cache_DBA[DBA_index].data = data;
    } else {
        // Read the data out of the cache block into the packet
        data = cache_DBA[DBA_index].data;
    } 
    // else {
    //     perror("Unknown packet type!");
    // }
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
 *      3.2     Invalidate an eventual entry in the B-TLB that points to b
 *      3.3     if (b's DUT entry LF field indicates the b holds a BTH table)
 *      3.3.1       Invalidate DUT entries associated with b's children
 *      3.4     else if (b's DUT entry dirty bit D==true)
 *      3.4.1       Save b's contents into physical memory
 *      4.  Install block level N+1
 *      5.  if (++N < data block level) goto 1
 */
void insert(Addr address, uint8_t *data)
{
    uint32_t last_BTH;
    // The packet should be aligned.
    // assert(pkt->getAddr() ==  pkt->getBlockAddr(blockSize));
    // The pkt should be a response
    // assert(pkt->isResponse());
    // Address should not be valid in the Cache. Set last valid BTH index.
    assert(!CacheSearch(address, last_BTH));
    // The address should not be in the TLB
    assert(cache_TLB.find(address) == cache_TLB.end());

    size_t i;
    uint32_t smallest_r_idx = -1;
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
                if(cache_DBA[VBIR].dut.R < smallest_r_idx)
                    smallest_r_idx = VBIR;
                cache_DBA[VBIR].dut.R = 0;
            }
            i++;
        }
        
        VBIR++;
        if(VBIR>capacity)
            VBIR=0;
    }

    // Select smallest R value if no suitable found in Maximum NUmber of Attempts
    if(i == MNA)
        VBIR = smallest_r_idx;
    
    // Make the BTH entry in level N point to b and set valid
    ((BTH_entry*)(cache_DBA[last_BTH].data))->I = VBIR;
    ((BTH_entry*)(cache_DBA[last_BTH].data))->V = true;

    // If b was valid
    if(cache_DBA[VBIR].dut.PV && cache_DBA[last_BTH].dut.V)
    {
        // Invalidate the entry of the BTH table that points to b
        cache_DBA[cache_DBA[VBIR].tt.PT].dut.V = false;

        // Invalidate an eventual entry in the B-TLB that points to b
        //TODO: BTH in TLB
        auto it = cache_TLB.find(address/blockSize);
        if (it != cache_TLB.end()) {
            cache_TLB.erase(it);
        }

        // if (b's DUT entry LF field indicates the b holds a BTH table)
        if(cache_DBA[VBIR].dut.LF < num_BTH)
        {
            for (size_t i = 0; i < blockSize/2; i++)
            {
                // Invalidate DUT entries associated with b's children
                if(((BTH_entry*)(cache_DBA[VBIR].data))->V)
                {
                    cache_DBA[((BTH_entry*)(cache_DBA[VBIR].data))->I].dut.PV = false;
                }
            }
        }
        // else if (b's DUT entry dirty bit D==true)
        else if(cache_DBA[VBIR].dut.D)
        {
            // Save b's contents into physical memory
            // Create a new request-packet pair
            // RequestPtr req = std::make_shared<Request>(
            //     cache_DBA[VBIR].tt.TAG, blockSize, 0, 0);

            // PacketPtr new_pkt = new Packet(req, MemCmd::WritebackDirty, blockSize);
            // new_pkt->dataDynamic(cache_DBA[VBIR].data); // This will be deleted later

            // DPRINTF(DbrcCache, "Writing packet back %s\n", pkt->print());
            // // Send the write to memory
            // memPort.sendPacket(new_pkt);
        }
    }

    // Install block level N+1
    // free(cache_DBA[VBIR].data);


    // if (++N < data block level) goto 1
    
    // DPRINTF(DbrcCache, "Inserting %s\n", pkt->print());
    // DDUMP(DbrcCache, pkt->getConstPtr<uint8_t>(), blockSize);

    // // Allocate space for the cache block data
    // uint8_t *data = new uint8_t[blockSize];

    // // Insert the data and address into the cache store
    // cacheStore[pkt->getAddr()] = data;

    // // Write the data into the cache
    // pkt->writeDataToBlock(data, blockSize);
}

int main()
{
    VBIR = 0;

    L0T_offset = blockSize;
    for(size_t i = 1; i < num_BTH; i++)
        L0T_offset *= (blockSize/2);
    cache_L0T = new BTH_entry[(1UL<<32)/(L0T_offset)];
    cache_DBA = new DBA_entry[capacity];

    delete [] cache_L0T;
    delete [] cache_DBA;
}