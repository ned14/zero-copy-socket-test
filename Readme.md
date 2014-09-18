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


### Real 1Gbps NIC: ###
Windows => Linux sees 60 Mb/sec with 2 thread and 4 buffer @ 65487.

Linux => Windows sees 0.1 Mb/sec with 2 thread and 4 buffer @ 65487. **Something is very wrong here**.
