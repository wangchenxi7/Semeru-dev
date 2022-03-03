#! /bin/bash

mode=$1
version=$2

if [ -z "${mode}"  ]
then
	echo " Please choose the operation : configure, build"
	read mode
fi

if [ -z "${version}"  ]
then
	echo " Please choose the version : slowdebug, fastdebug, release"
	read version
fi

## Saint check
if [ -z "$HOME"  ]
then
	echo "HOME is null. Please configure it."
	exit
fi

home_dir="$HOME"

# Both available cores && number of jobs
num_core=`nproc --all`
# in MB, 32GB as default
build_mem="32768"
boot_jdk="${home_dir}/jdk-12.0.2"


## Do the action

if [ "${mode}" = "configure"  ]
then
	## Add RDMA dependency
	./configure --prefix=${home_dir}/jdk12u-self-build  --with-debug-level=${version}   --with-extra-cxxflags="-lrdmacm -libverbs" --with-extra-ldflags="-lrdmacm -libverbs"  --with-num-cores=${num_core} --with-jobs=${num_core} --with-memory-size=${build_mem} --with-boot-jdk=${boot_jdk} 
elif [ "${mode}" = "build"  ]
then
	# The job number is decided in configuration by --with-boot-jdk=${boot_jdk}
	make CONF=linux-x86_64-server-${version}
elif [ "${mode}" = "install"  ]
then
	make install CONF=linux-x86_64-server-${version}
else
	echo " !! Wrong build mode slected. !!"
	exit 
fi
