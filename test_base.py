#include <cstdint>
#include <cstdio>
#include <cassert>
#include <map>
#include <cstring>

#include <iostream>
#include <fstream>
#include <string>

class BTH_entry:
    V = 0
    I = 0

class DUT_entry:
    V = False
    D = False
    L = False
    LF = 0
    PV = False
    R = 0

class TT_entry:
    TAG = 0
    PT = 0

class DBA_entry:
    def __init__(self):
        self.data = []
        self.dut = DUT_entry()
        self.tt = TT_entry()

# Consts 
blockSize = 64
capacity = int((1<<15)//blockSize)
target_BTH = 3
num_BTH = 3
TLB_size = (1<<16)
MNA = 5

L0T_offset = blockSize*((blockSize/2)**(num_BTH-1))
cache_TLB = {}
VBIR = 0
cache_L0T = [BTH_entry() for _ in range(int(((1<<32)//L0T_offset)))]
cache_DBA = [DBA_entry() for _ in range(capacity)]


def pow(x, e):
    y = x
    for i in range(1,e):
        y *= x
    return y

def CacheSearch(block_addr):
    offset = blockSize/2
    index = 0
    
    if(cache_L0T[int(block_addr//L0T_offset)].V):
        index = cache_L0T[int(block_addr//L0T_offset)].I
    else:
        index = -1
        return False, index

    for i in range(1,num_BTH):
        entries = cache_DBA[index].data
        idx = int(block_addr//(L0T_offset//(offset))) & int(blockSize/2-1)
        if(entries[idx].V):
            index = entries[idx].I
            if(cache_DBA[index].dut.R < 32):
                cache_DBA[index].dut.R += 1
        else:
            return False, index
        offset *= blockSize/2

    # Validate data DUT entry
    if(cache_DBA[index].dut.LF != num_BTH or cache_DBA[index].dut.V == False or cache_DBA[index].tt.TAG != int(block_addr//blockSize)):
        return False, index

    return True, index

# brief Check if address exists in cache. Get/Set data if in cache.
def accessFunctional(block_addr, data, isWrite):

    found, DBA_index = CacheSearch(block_addr)
    
    # TLB Search
    if int(block_addr//blockSize) in cache_TLB:
        DBA_index = cache_TLB[int(block_addr//blockSize)]
    elif found:
        # Full Cache Search
        # Write cache find to TLB
        if (len(cache_TLB) > TLB_size):
            it = list(cache_TLB)[0]
            cache_TLB.pop(it)
        
        # TODO: implement storing BTH or data in TLB
        cache_TLB[int(block_addr//blockSize)] = DBA_index
    else:
        return False, data
    

    # Perform Operation on found cache block
    if (isWrite):
        # Write the data into the block in the cache
        cache_DBA[DBA_index].data[block_addr&(blockSize-1)] = data
    else:
        # Read the data out of the cache block into the packet
        data = cache_DBA[DBA_index].data[block_addr&(blockSize-1)]
    return True, data

# /**
#  * @brief Insert data in to cache after memory response. Handle write-back and replacement policy.
#  * 
#  * @details 
#  *      1.  b = Select a DBA victim block
#  *      2.  Make the BTH entry in level N point to b
#  *      3.  if (b's DUT entry bits V==true and PV==true)
#  *      3.1     Invalidate the entry of the BTH table that points to b
#  *      3.2     Invalidate an eventual entry in the B-TLB that points to b
#  *      3.3     if (b's DUT entry LF field indicates the b holds a BTH table)
#  *      3.3.1       Invalidate DUT entries associated with b's children
#  *      3.4     else if (b's DUT entry dirty bit D==true)
#  *      3.4.1       Save b's contents into physical memory
#  *      4.  Install block level N+1
#  *      5.  if (++N < data block level) goto 1
#  */
def insert(address, data):
    last_BTH = 0
    current_level = 0
    global VBIR
    global cache_TLB 
    global cache_L0T
    global cache_DBA

    # The packet should be aligned.
    # assert(pkt->getAddr() ==  pkt->getBlockAddr(blockSize));
    # The pkt should be a response
    # assert(pkt->isResponse());
    # Address should not be valid in the Cache. Set last valid BTH index.
    found, last_BTH = CacheSearch(address)
    assert(not found)
    # The address should not be in the TLB
    assert(int(address//blockSize) not in cache_TLB)

    # // Miss in L0T
    if (last_BTH == -1):
        # // Invalidate DUT entries associated with b's children
        if(cache_L0T[int(address//L0T_offset)].V):
            cache_DBA[cache_L0T[int(address//L0T_offset)].I].dut.PV = False

        current_level = 0
    else:
        current_level = cache_DBA[last_BTH].dut.LF

    current_level += 1

    while(current_level <= num_BTH):
        i = 0
        smallest_r_idx = -1
        # // Select DBA vitim block
        while(i < MNA):
            if(not cache_DBA[VBIR].dut.L):
                if (not cache_DBA[VBIR].dut.V or
                    not cache_DBA[VBIR].dut.PV or
                    cache_DBA[VBIR].dut.R == 0):
                    break
                else:
                    if(cache_DBA[VBIR].dut.R < smallest_r_idx):
                        smallest_r_idx = VBIR
                    cache_DBA[VBIR].dut.R = 0
                i += 1
            
            VBIR += 1
            if(VBIR>=capacity):
                VBIR=0;

        # // Select smallest R value if no suitable found in Maximum Number of Attempts
        if(i == MNA):
            VBIR = smallest_r_idx
        
        if (current_level == 1):
            # // Make the BTH entry in L0T point to b and set valid
            cache_L0T[int(address//L0T_offset)].I = VBIR
            cache_L0T[int(address//L0T_offset)].V = True
        else:
            # // Make the BTH entry in level N point to b and set valid
            cache_DBA[last_BTH].data[int(address//(L0T_offset/pow(blockSize/2, current_level-1)))&int(blockSize/2-1)].I = VBIR
            cache_DBA[last_BTH].data[int(address//(L0T_offset/pow(blockSize/2, current_level-1)))&int(blockSize/2-1)].V = True

        if (VBIR==1):
            x=1
        

        # // If b was valid
        if(cache_DBA[VBIR].dut.V == True and cache_DBA[VBIR].dut.LF > 0):
            if(cache_DBA[VBIR].dut.PV == True):
                # // Invalidate the entry of the BTH table that points to b
                if(cache_DBA[VBIR].dut.LF == 1):
                    cache_L0T[cache_DBA[VBIR].tt.PT].V = False;
                else:
                    for i in range(blockSize//2):
                        if (cache_DBA[cache_DBA[VBIR].tt.PT].data[i].I == VBIR):
                            cache_DBA[cache_DBA[VBIR].tt.PT].data[i].V = False
                            break
            
            # // If is data, invalidate an entry in the B-TLB that points to b
            # //TODO: BTH in TLB
            if (cache_DBA[VBIR].dut.LF == num_BTH):
                if (int(address//blockSize) in cache_TLB):
                    cache_TLB.pop(int(address//blockSize))
                cache_DBA[VBIR].tt.TAG = 0

            # // if (b's DUT entry LF field indicates the b holds a BTH table)
            if(cache_DBA[VBIR].dut.LF < num_BTH):
                for i in range(blockSize//2):
                    # // Invalidate DUT entries associated with b's children
                    if(cache_DBA[VBIR].data[i].V):
                        cache_DBA[((cache_DBA[VBIR].data))[i].I].dut.PV = False
            # // else if (b's DUT entry dirty bit D==true)
            elif(cache_DBA[VBIR].dut.D):
                # // Save b's contents into physical memory
                # // Create a new request-packet pair
                # // RequestPtr req = std::make_shared<Request>(
                # //     cache_DBA[VBIR].tt.TAG, blockSize, 0, 0);

                # // PacketPtr new_pkt = new Packet(req, MemCmd::WritebackDirty, blockSize);
                # // new_pkt->dataDynamic(cache_DBA[VBIR].data); // This will be deleted later

                # // DPRINTF(DbrcCache, "Writing packet back %s\n", pkt->print());
                # // // Send the write to memory
                # // memPort.sendPacket(new_pkt);
                pass

        # // Install block level N+1
        # // Clear data memory
        if (current_level == num_BTH):
            cache_DBA[VBIR].data =  [0 for _ in range(blockSize)]
        else:
            cache_DBA[VBIR].data = [BTH_entry() for _ in range(int(blockSize//2))]
        
        cache_DBA[VBIR].dut.V = True
        cache_DBA[VBIR].dut.PV = True
        cache_DBA[VBIR].dut.LF = current_level
        cache_DBA[VBIR].dut.R = 1
        if (current_level == 1):
            cache_DBA[VBIR].tt.PT = int(address//L0T_offset)
        else:
            cache_DBA[VBIR].tt.PT = last_BTH

        last_BTH = VBIR
        current_level += 1
        VBIR += 1
        if(VBIR>=capacity):
            VBIR=0;

        # // if (++N < data block level) goto 1

    # // DPRINTF(DbrcCache, "Inserting %s\n", pkt->print());
    # // DDUMP(DbrcCache, pkt->getConstPtr<uint8_t>(), blockSize);

    cache_DBA[last_BTH].tt.TAG = int(address//blockSize)

    cache_DBA[last_BTH].data[(address&(blockSize-1))] = data

    # // Write cache find to TLB
    if (len(cache_TLB) > TLB_size):
        it = list(cache_TLB)[0]
        cache_TLB.pop(it)
    
    # // TODO: implement storing BTH or data in TLB
    cache_TLB[int(address//blockSize)] = last_BTH

    # // // Write the data into the cache
    # // pkt->writeDataToBlock(cache_DBA[VBIR].data, blockSize);

data = 42
data2 = 0

trace = open("trace")
misses = 0
total = 0

for line in trace:
    addr = int(line[2:], 16);
    total += 1
    if(total==74720):
        pass
    
    found, data2 = accessFunctional(addr, data2, False)
    if (not found):
        insert(addr, data);
        success, data2 = accessFunctional(addr, data2, False)
        assert(data==data2);
        misses += 1

trace.close()

print(str(misses)+"/"+str(total))
