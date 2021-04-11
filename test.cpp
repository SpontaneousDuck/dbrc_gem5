#include <cstdint>
#include <cstdio>
#include <cassert>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstring>

#include <iostream>
#include <fstream>
#include <string>

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
  BTH_entry* BTH;
  uint8_t* data;
  DUT_entry dut;
  TT_entry tt;
} DBA_entry;

/// The block size for the cache
const unsigned blockSize = 64;
/// Number of blocks in the cache (size of cache / block size)
const unsigned capacity = (1<<20)/blockSize;
const unsigned target_BTH = 5;
const unsigned num_BTH = 3;
const unsigned TLB_size = (1<<16);
const unsigned MNA = 5;
uint32_t L0T_offset; 

/// TLB buffer. Unordered map since fully-associative
std::unordered_map<uint32_t, uint32_t> cache_TLB;
std::vector<uint32_t> cache_TLB_order;
uint32_t VBIR; 
BTH_entry* cache_L0T;
DBA_entry* cache_DBA;

uint32_t pow(uint32_t x, uint32_t e)
{
    uint32_t y = x;
    for (size_t i = 1; i < e; i++)
    {
        y *= x;
    }
    return y;
}

bool CacheSearch(Addr block_addr, uint32_t &index)
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
bool accessFunctional(Addr block_addr, uint8_t *data, bool isWrite)
// bool accessFunctional(PacketPtr pkt)
{
    uint32_t DBA_index = 0;
    // Addr block_addr = pkt->getBlockAddr(blockSize);
    
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
    if (isWrite) {
        // Write the data into the block in the cache
        cache_DBA[DBA_index].data[block_addr&(blockSize-1)] = *data;
    } else {
        // Read the data out of the cache block into the packet
        *data = cache_DBA[DBA_index].data[block_addr&(blockSize-1)];
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
    uint32_t last_BTH, current_level;
    // The packet should be aligned.
    // assert(pkt->getAddr() ==  pkt->getBlockAddr(blockSize));
    // The pkt should be a response
    // assert(pkt->isResponse());
    // Address should not be valid in the Cache. Set last valid BTH index.
    assert(!CacheSearch(address, last_BTH));
    // The address should not be in the TLB
    assert(cache_TLB.find(address/blockSize) == cache_TLB.end());

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
                        smallest_r_idx = VBIR;
                        smallest_r = cache_DBA[VBIR].dut.R;
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
                // RequestPtr req = std::make_shared<Request>(
                //     cache_DBA[VBIR].tt.TAG, blockSize, 0, 0);

                // PacketPtr new_pkt = new Packet(req, MemCmd::WritebackDirty, blockSize);
                // new_pkt->dataDynamic(cache_DBA[VBIR].data); // This will be deleted later

                // DPRINTF(DbrcCache, "Writing packet back %s\n", pkt->print());
                // // Send the write to memory
                // memPort.sendPacket(new_pkt);
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
            cache_DBA[last_BTH].BTH[(address/(L0T_offset/pow(blockSize/2, current_level-1)))&(blockSize/2-1)].I = VBIR;
            cache_DBA[last_BTH].BTH[(address/(L0T_offset/pow(blockSize/2, current_level-1)))&(blockSize/2-1)].V = true;
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

    cache_DBA[last_BTH].tt.TAG = address/blockSize;

    cache_DBA[last_BTH].data[(address&(blockSize-1))] = *data;

    // Write cache find to TLB
     if (cache_TLB.size() > TLB_size)
    {
        cache_TLB.erase(cache_TLB_order.front());
        cache_TLB_order.erase(cache_TLB_order.begin());
    }
    
    // TODO: implement storing BTH or data in TLB
    cache_TLB[address/blockSize] = last_BTH;
    cache_TLB_order.push_back(address/blockSize);

    // // Write the data into the cache
    // pkt->writeDataToBlock(cache_DBA[VBIR].data, blockSize);


}

void init()
{
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

void clean()
{
    for (size_t i = 0; i < capacity; i++)
    {
        free(cache_DBA[i].data);
    }
    
    free(cache_L0T);
    free(cache_DBA);
}

int main()
{
    init();

    uint32_t addr = 0x2022208;
    uint8_t data = 42;
    uint8_t data2 = 0;

    // insert(addr, &data);
    // accessFunctional(addr&(!(blockSize-1)), &data2, true);


    std::ifstream trace("trace");
    std::string address;
    bool success;
    uint64_t misses = 0;
    uint64_t total = 0;

    while (std::getline(trace, address)) {
        addr = std::stoul(address.substr(2), 0 ,16);
        total++;
        if(total==74720)
            uint32_t x = 0;

        if (!accessFunctional(addr, &data2, false))
        {
            insert(addr, &data);
            success = accessFunctional(addr, &data2, false);
            assert(data==data2);
            misses++;
        }
    }

    trace.close();

    clean();
}