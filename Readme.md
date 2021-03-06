On my 4 core 8 thread 3.5Ghz i7-3770K testing ASIO's ability to maximise UDP send and receive. Firstly, iperf was run like this:

A: iperf -s -u -l 65507
B: iperf -c 127.0.0.1 -u -l 65507 -b 1000G

Linux reports maximum UDP loopback throughput of ? Mb/sec
  and maximum NIC throughput of 1.13 Gb/sec (9.10 Gbit/sec). 
Win8 reports maximum UDP loopback throughput of 2.9 Gb/sec (23.2 Gbit/sec)
  and maximum NIC throughput of 0.32 Gb/sec (2.59 Gbit/sec). 


### Loopback: ###
Linux maxes out at 6881 Mb/sec with 2 thread and 1 buffer @ 65507.

Windows maxes out at 3000 Mb/sec with 2 thread and 4 buffer @ 65507.


### Real 1Gbps NIC MTU 9000: ###
MTU of 8972 was confirmed using:

Win8: ping -f -l 8972 addr
Linux: ping -M do -s 8972 addr

Windows => Linux sees 35 Mb/sec with 2 thread and 4 buffer @ 65487. This may be the notoriously awful Intel 82579V NIC on the Windows machine though.

Linux => Windows sees 107 Mb/sec with 2 thread and 4 buffer @ 65487.

#### Next tried multiples of MTU ... ####

Windows => Linux sees 101 Mb/sec with 2 thread and 1 buffer @ 8972 (1 MTU)
Linux => Windows sees 118 Mb/sec with 2 thread and 4 buffer @ 8972 (1 MTU)
Windows machine @ 11% CPU
Linux machine @ 28% CPU (note this is routed through OpenVZ networking)

Windows => Linux sees 117 Mb/sec with 2 thread and 1 buffer @ 4000 (0.5 MTUs)
Linux => Windows sees at 117 Mb/sec with 2 thread and 4 buffer @ 4000 (0.5 MTUs)
Windows machine @ 15% CPU
Linux machine @ 28% CPU (note this is routed through OpenVZ networking)

Windows => Linux sees 86 Mb/sec with 1 thread and 1 buffer @ 8972 (1 MTU)
Linux => Windows sees 118 Mb/sec with 1 thread and 4 buffer @ 8972 (1 MTU)
Windows machine @ 10% CPU
Linux machine @ 13% CPU (note this is routed through OpenVZ networking)

Windows => Linux sees 115 Mb/sec with 1 thread and 1 buffer @ 4000 (0.5 MTUs)
Linux => Windows sees at 117 Mb/sec with 1 thread and 4 buffer @ 4000 (0.5 MTUs)
Windows machine @ 12% CPU
Linux machine @ 17% CPU (note this is routed through OpenVZ networking)

### CPU usage: ###

Instead of performance, can zero copy reduce CPU utilisation?

#### Linux: ####
For 1500 byte UDP packets over a Gigabit ethernet link, I see these whole system load factors on Linux:

Linux => Linux without splice(): 0.90
Linux => Linux with splice(): 0.92

With memory pressure so effects of L3 cache negated:

Linux => Linux without splice(): 1.67
Linux => Linux with splice(): 1.83

Therefore for 1500 byte packets, *it makes zero sense to use splice() on Linux*. For 4k UDP packets:

Linux => Linux without splice(): 0.57
Linux => Linux with splice(): 0.54

With 4k UDP packets it begins to make sense. I'd assume it's all gravy after this as packet sizes increase.

#### Windows: ####

For 1500 byte UDP packets over a Gigabit ethernet link, I see these whole system CPU usage on Windows 8.1:

Windows => Linux without dual buffer cycling (i.e. with copy): 17%
Windows => Linux with dual buffer cycling (i.e. zero copy): 17%

For 4k UDP packets:

Windows => Linux without dual buffer cycling (i.e. with copy): 14%
Windows => Linux with dual buffer cycling (i.e. zero copy): 14%

I suspect this is because NetDMA is not supported on Windows 8 and therefore Intel QuickData is not available.

