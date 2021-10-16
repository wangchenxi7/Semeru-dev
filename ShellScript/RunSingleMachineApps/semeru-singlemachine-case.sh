#! /bin/bash

bench=$1

if [ -z "${bench}"  ]
then
	echo "Please input the bench : e.g.  Simple"
	read bench
fi


mode=$2

if [ -z "${mode}"  ]
then
	echo "Please input the execution mode: gdb, execution"
	read mode
fi

#
# Parameters
#

###
# File to be executed
BenchPath="${HOME}/System-Dev-Testcase/Semeru/CPUServer"
#EXEC_JAVA_HOME="/mnt/ssd/wcx/JDK/build/linux-x86_64-server-fastdebug/jdk"
EXEC_JAVA_HOME="${HOME}/Semeru-dev/CPU-Server/build/linux-x86_64-server-slowdebug/jdk"


# Java path follow the JAVA_HOME

##
# Semreu Parameters


# Enable the Semeru Memory pool
EnableSemeruMemPool="true"
#EnableSemeruMemPool="false"

SemeruMemPoolSize="8G"
#SemeruMemPool=""

# Region size and Heap's allocation alignment.
#SemeruMemPoolAlignment="1G"
SemeruMemPoolAlignment="4G"




##
# Original jdk parameters
MemSize="1024M"

#RegionSize="64m"
RegionSize="512m"
TLABSize="4096"

STWParallelThread=1
concurrentThread=1

compressedOop="no"
#compressedOop="yes"


#logOpt="-Xlog:gc+heap=debug"
#logOpt="-Xlog:gc=info"

# Print GC, Concurrent Marking details
#logOpt="-Xlog:gc+marking=debug"
#logOpt="-Xlog:gc,gc+marking=debug"

# heap is a self defined Xlog tag.
#logOpt="-Xlog:heap=debug,gc=debug,gc+marking=debug,gc+remset=debug,gc+ergo+cset=debug,gc+bot=debug,semeru+alloc=debug, semeru+mem_trace=debug"
logOpt="-Xlog:heap=debug,gc=debug,gc+marking=debug,semeru+alloc=debug,semeru+rdma=debug,semeru+mem_trace=debug,semeru+compact=debug"


#
# Apply the options.
#


if [ ${EnableSemeruMemPool} = "false" ]
then
	SemeruMemPoolParameter=""
elif  [ ${EnableSemeruMemPool} = "true" ]
then
	#SemeruMemPoolParameter="-XX:SemeruEnableMemPool -XX:SemeruMemPoolMaxSize=${SemeruMemPoolSize} -XX:SemeruMemPoolInitialSize=${SemeruMemPoolSize} -XX:SemeruMemPoolAlignment=${SemeruMemPoolAlignment} "
	SemeruMemPoolParameter="-XX:SemeruEnableMemPool"
else
	echo "Wrong vlaue for 'EnableSemeruMemPool'"
	exit
fi



if [ ${compressedOop} = "no"  ]
then
	compressedOop="-XX:-UseCompressedOops"	

elif [ ${compressedOop} = "yes"  ]
then
	# Open the compressed oop, no matter what's  the size of the Java heap
	compressedOop="-XX:+UseCompressedOops"

else
	# Used the default policy
	# If Heap size <= 32GB, use Compressed oop, 32 bits addresso. 
	compressedOop=""
fi




##
# Other JVM options
other_opt="-XX:NewRatio=3"




###
# Functions

close_last_dead_pid () {
	echo "Try to close the dead processes."

	# Close dead pid 1
	pid1=`ps aux | grep "java" | grep -v "grep"  | awk '{print $2}' | awk 'NR==1 {print $0}' `
	# the grep process
	#pid2=`ps aux | grep "java" | awk '{print $2}' | awk 'NR==2 {print $0}' `
	
	echo "pid1 ${pid1}"
	if [ -n "${pid1}" ]
	then 
		echo "close ${pid1} "
		sudo kill -9 ${pid1}
	fi

	# Close dead pid 2	
	# it contain 3 process, last one, current one, and the grep
	current_pid="$$"
	ps aux | grep "debug_cpu_server.sh" | grep -v "grep"  > ~/tmp.txt
	pid1=`cat ~/tmp.txt | awk '{print $2}' | awk 'NR==1 {print $0}' `
	pid2=`cat ~/tmp.txt | awk '{print $2}' | awk 'NR==2 {print $0}' `

	echo "current_pid ${current_pid}, pid1 ${pid1}, pid2 ${pid2}"
	if [ "${pid1}" = "${current_pid}" ]
	then 
		echo "Kill pid ${pid2}"
		sudo kill -9 ${pid2}
	else
		echo "Kill pid ${pid1}"
		sudo kill -9 ${pid1}
	fi

}



#
# Execute the  Commandlines 
#

if [ "${mode}" = "gdb"  ]
then
	echo "gdb --args  ${EXEC_JAVA_HOME}/bin/java -XX:+UseG1GC  ${compressedOop}  ${logOpt}  -XX:G1HeapRegionSize=${RegionSize} -XX:TLABSize=${TLABSize}   -Xms${MemSize} -Xmx${MemSize}   ${SemeruMemPoolParameter}  -XX:ParallelGCThreads=${STWParallelThread}   -XX:ConcGCThreads=${concurrentThread} -cp ${BenchPath}  ${bench}"
	gdb --args  ${EXEC_JAVA_HOME}/bin/java -XX:+UseG1GC  ${compressedOop}  ${logOpt}  -XX:G1HeapRegionSize=${RegionSize} -XX:TLABSize=${TLABSize}   -Xms${MemSize} -Xmx${MemSize}   ${SemeruMemPoolParameter}  -XX:ParallelGCThreads=${STWParallelThread}   -XX:ConcGCThreads=${concurrentThread} ${other_opt}  -cp ${BenchPath} ${bench}
elif [ "${mode}" = "execution" ]
then
	# Close the old one
	close_last_dead_pid	
	
	# Run a new bench
	echo "${EXEC_JAVA_HOME}/bin/java -XX:+UseG1GC  ${compressedOop}  ${logOpt} -XX:G1HeapRegionSize=${RegionSize} -XX:TLABSize=${TLABSize}   -Xms${MemSize} -Xmx${MemSize} ${SemeruMemPoolParameter}  -XX:ParallelGCThreads=${STWParallelThread}   -XX:ConcGCThreads=${concurrentThread}  -cp ${BenchPath} ${bench}"
	${EXEC_JAVA_HOME}/bin/java -XX:+UseG1GC  ${compressedOop}  ${logOpt}  -XX:G1HeapRegionSize=${RegionSize} -XX:TLABSize=${TLABSize}   -Xms${MemSize} -Xmx${MemSize} ${SemeruMemPoolParameter}  -XX:ParallelGCThreads=${STWParallelThread}   -XX:ConcGCThreads=${concurrentThread} ${other_opt}  -cp ${BenchPath}  ${bench}

else
	echo "Wrong Mode."
fi



