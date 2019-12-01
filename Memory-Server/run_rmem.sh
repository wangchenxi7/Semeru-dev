#! /bin/bash



# Testcase
if [ -z "${HOME}"  ]
then
	echo "HOME directory is NULL.	Please set it correctly."
	exit
fi

testcase_dir="${HOME}/testcase/Semeru/RemoteMemory"
bench=$1

if [ -z "${bench}"  ]
then
	echo "Inpute the bencmark name. e.g.  Case1"
	read bench
else
	echo "Run the benchmark ${testcase}${bench} "
fi


# JVM configuration

## RDMA Buffer size + Memory server reserved size, i.e. 1GB.
mem="33g"
gc_mode="-XX:+UseG1GC"

#disable compressed oops
oop_mode="-XX:-UseCompressedOops"

#log_mode="-Xlog:heap=debug,gc+marking=debug"
log_mode="-Xlog:heap=debug"
#log_mode=""

## Do  the excution

echo "java  ${gc_mode} ${oop_mode}   -Xms${mem} -Xmx${mem} ${log_mode}  -cp ${testcase_dir} ${bench}"
java  ${gc_mode} ${oop_mode}   -Xms${mem} -Xmx${mem} ${log_mode}  -cp ${testcase_dir} ${bench}
