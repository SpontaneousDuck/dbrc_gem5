# import the m5 (gem5) library created when gem5 is built
import m5
# import all of the SimObjects
from m5.objects import *

class L1ICache(Cache):
    size = '64kB'
    assoc = 2
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 4
    tgts_per_mshr = 20

    def __init__(self, options=None):
        super(L1ICache, self).__init__()
        pass

class L1DCache(Cache):
    size = '64kB'
    assoc = 2
    tag_latency = 3
    data_latency = 3
    response_latency = 3
    mshrs = 4
    tgts_per_mshr = 20

    def __init__(self, options=None):
        super(L1DCache, self).__init__()
        pass

class L2StandardCache(Cache):
    size = '1MB'
    assoc = 16
    tag_latency = 9
    data_latency = 9
    response_latency = 9
    mshrs = 32
    tgts_per_mshr = 20

    def __init__(self, options=None):
        super(L2StandardCache, self).__init__()
        pass

class L2DbrcCache(DbrcCache):
    latency = 1
    size = '64kB'
    num_BTH = 3
    TLB_size = 65536
    MNA = 5
    target_BTH = 3
    
    def __init__(self, options=None):
        super(L2DbrcCache, self).__init__()
        pass

binary = '/usr/local/src/gem5/tests/test-progs/hello/bin/x86/linux/hello'

# create the system we are going to simulate
system = System()

# Set the clock fequency of the system (and all of its children)
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = '1GHz'
system.clk_domain.voltage_domain = VoltageDomain()

# Set up the system
system.mem_mode = 'timing'               # Use timing accesses
system.mem_ranges = [AddrRange('512MB')] # Create an address range

# Create a simple CPU
system.cpu = TimingSimpleCPU()


# Create a simple cache
system.cpu.icache = L1ICache()
system.cpu.dcache = L1DCache()
system.cpu.icache_port = system.cpu.icache.cpu_side
system.cpu.dcache_port = system.cpu.dcache.cpu_side

# Create a memory bus, a coherent crossbar, in this case
system.l2bus = L2XBar()

# Hook the L1 cache up to the L2 bus
system.cpu.icache.mem_side = system.l2bus.cpu_side_ports
system.cpu.dcache.mem_side = system.l2bus.cpu_side_ports

# system.l2cache = L2StandardCache()
system.l2cache = L2DbrcCache()
system.l2cache.cpu_side = system.l2bus.mem_side_ports

# Create a memory bus, a coherent crossbar, in this case
system.membus = SystemXBar()

system.l2cache.mem_side = system.membus.cpu_side_ports

# create the interrupt controller for the CPU and connect to the membus
system.cpu.createInterruptController()
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports

# Create a DDR3 memory controller and connect it to the membus
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

# Connect the system up to the membus
system.system_port = system.membus.cpu_side_ports

# Create a process for a simple "Hello World" application
process = Process()
# Set the command
# grab the specific path to the binary
binpath = binary
# cmd is a list which begins with the executable (like argv)
process.cmd = [binpath]
# Set the cpu to use the process as its workload and create thread contexts
system.cpu.workload = process
system.cpu.createThreads()

# system.workload = SEWorkload.init_compatible(binpath)

# set up the root SimObject and start the simulation
root = Root(full_system = False, system = system)
# instantiate all of the objects we've created above
m5.instantiate()

print("Beginning simulation!")
exit_event = m5.simulate()
print('Exiting @ tick %i because %s' % (m5.curTick(), exit_event.getCause()))