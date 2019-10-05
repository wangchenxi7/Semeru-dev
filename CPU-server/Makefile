# Module build
# 
#  The basic idea is to use the Makefile of Kernel Source to do the compilation.
#  This module needs some external variable defined in kernel, or other kernel module.
#	So we need to use the Module.SYSVERS
#
#	This testcase is built as the C based Octopus platform.
#	For the client end,  it works like a normal swap partitions. 
#	All the I/O requst written  into the swap partition will be forwarded to remote memory pool.
#	The remote memory pool is C based now.
#	Finally, we need to replace the remote server as a Java based memory pool.
#


kernel_rdma_module_dir=/lib/modules/$(shell uname -r)/extra/octopus_client


#ccflags-y := -v
#NOSTDINC_FLAGS += -I/usr/src/ofa_kernel/default/include

# The Modules.sysvers listed here will be copy to bulding directory
#KSYMVERS = /mnt/ssd/wcx/testcase/IB-test-case/RDMA_kernel_post_send_recv/Module.symvers /usr/src/ofa_kernel/default/Module.symvers



# obj-[m] means compile this module but not link it into kernel at startup.
obj-m += octopus_client.o

octopus_client-y	:= register_disk.o	# main entry
octopus_client-y 	+= rdma_client.o	# merged library


all:
	# -C change to kernel source code, to use its Makefile 
	#  M is a hythen, return to $(PWD) after rading the Make in /lib/modules/$(shell uname -r)/build
	# make  -C /lib/modules/$(shell uname -r)/build  SUBDIRS=$(PWD)  KBUILD_EXTRA_SYMBOLS="$(KSYMVERS)"  modules
	make  -C /lib/modules/$(shell uname -r)/build  M=$(PWD)  modules
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install:
	mkdir -p $(kernel_rdma_module_dir)
	/usr/bin/install -c -m 644 ./octopus_client.ko $(kernel_rdma_module_dir)

uninstall:
	modprobe -r octopus_client
	rm $(kernel_rdma_module_dir)/octopus_client.ko