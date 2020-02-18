#! /bin/bash



### Configurations

# Device
target_dev="/dev/rmempool"

#Parallel
thread_num="1"

# Benchmark type
#testbench="randread"
testbench="randwrite"


# Execution time
runtime="1"


### Do the action

	echo "sudo fio --bs=64k --numjobs=${thread_num} --iodepth=4 --loops=1 --ioengine=libaio --direct=1 --invalidate=1 --fsync_on_close=1 --randrepeat=1 --norandommap --time_based --runtime=${runtime} --filename=${target_dev} --name=read-phase --rw=${testbench}"

	sudo fio --bs=64k --numjobs=${thread_num} --iodepth=4 --loops=1 --ioengine=libaio --direct=1 --invalidate=1 --fsync_on_close=1 --randrepeat=1 --norandommap --time_based --runtime=${runtime} --filename=${target_dev} --name=read-write-test --rw=${testbench}



