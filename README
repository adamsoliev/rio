RIO - Rigid I/O
---------------

A command line utility for benchmarking modern SSDs using Linux io_uring.
RIO executes I/O workloads and reports performance metrics including IOPS,
latency, and throughput.


USAGE
-----

./rio --filename=/dev/ng0n1 --type=randomread --size=1g --iodepth=32 --bs=4k


PARAMETERS
----------

--filename  : Target device or file path (e.g., /dev/nvme0n1)
--type      : I/O pattern type (currently supports: randomread)
--size      : Total size of I/O workload (e.g., 1g, 512m, 2048k)
--iodepth   : Queue depth, number of concurrent I/O operations in flight
--bs        : Block size for each I/O operation (e.g., 4k, 8k, 128k)


OUTPUT
------

RIO reports the following metrics after completing the workload:

- IOPS: I/O operations per second
- Latency: Average, P50, P95, P99 latencies in microseconds
- Throughput: Bandwidth in MB/s


ROADMAP & CHANGELOG
-------------------

v0.1
  - Single-threaded execution
  - Random read workload pattern
  - io_uring-based I/O submission and completion
  - Basic performance metrics reporting
