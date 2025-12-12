#!/bin/bash
OUTPUT="fio_results.txt"
> $OUTPUT  # Clear the file

for workload in randread randwrite read write; do
  for engine in io_uring libaio psync; do
    for jobs in 1 4; do
      for depth in 1 32; do
        echo "=== rw=$workload ioengine=$engine numjobs=$jobs iodepth=$depth ===" | tee -a $OUTPUT
        sudo fio benchmark.fio \
          --rw=$workload \
          --ioengine=$engine \
          --numjobs=$jobs \
          --iodepth=$depth | tee -a $OUTPUT
        echo "" | tee -a $OUTPUT  # Add blank line between runs
      done
    done
  done
done

echo "All tests completed. Results saved to $OUTPUT"
