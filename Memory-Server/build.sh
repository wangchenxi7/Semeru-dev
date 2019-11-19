#! /bin/bash


## Build openjdk in SLOW-DEBUG mode
## Add RDMA dependency

mode=$1

if [ -z "${mode}"  ]
then
	echo " Please choose the operation : debug, product"
	read mode
fi

if [ "${mode}" = "debug"  ]
then
	./configure --prefix=/mnt/ssd/wcx/jdk12u-self-build  --with-debug-level=slowdebug   --with-extra-cxxflags="-lrdmacm -libverbs" --with-extra-ldflags="-lrdmacm -libverbs"

fi



