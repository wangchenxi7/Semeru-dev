# 1. Summary of Semeru

*Semeru* is a managed runtime built for a memorydisaggregated cluster where each managed application uses one CPU server and multiple memory servers. When launched on Semeru, the process runs its application code (mutator) on the CPU server, and the garbage collector on both the CPU server and memory servers in a coordinated manner. Due to task ofﬂoading and moving computation close to data, Semeru signiﬁcantly improves the locality for both the mutator and GC and, hence, the end-to-end performance of the application.

# 2. Setup environments

- **Hardware: Intel servers with InﬁniBand**
- **Run-time environment:OpenJDK 12.02, Linux-4.11-rc8, CentOS 7.5(7.6) with MLNX-OFED 4.3(4.5)**
- **Code licenses: The GNU General Public License (GPL)**

# 3. Description

## 3.1 *Semeru*'s Codebase

*Semeru* contains the following three components:

- the Linux kernel, which includes a modiﬁed swap system, block layer and a RDMA module

- the CPU-server Java Virtual Machine (JVM)

- the Memory-server lightweight Java Virtual Machine (LJVM)

  These three components and their relationships are illustrated in figure below:

 <img src="figures/semeru_ae_overview.pdf" alt="Alt " title="Overview of *Semeru*'s codebase" style="zoom:100%;" />

 

## 3.2 Deploying Semeru

To build *Semeru*, the ﬁrst step is to download its source code:

​	<code>git clone git@github.com:uclasystem/Semeru.git </code>

When deploying *Semeru*, install the three components in the following order: the kernel on the CPU server, the *Semeru* JVM on the CPU server, and the LJVM on each memory server. Finally, connect the CPU server with memory servers before running applications.



#### CPU Server Kernel Installation

We ﬁrst discuss how to build and install the kernel.

- Modify *grub* and set transparent_hugepage to *madvise*:

  <code>sudo vim /etc/default/grub <br />+ transparent_hugepage=madvise </code>

- Install the kernel and restart the machine:

  <code>cd Semeru/Linux-4.11-rc8<br>sudo ./build_kernel.sh build<br>sudo ./build_kernel.sh install<br>sudo reboot</code>	

- Install the MLNX-OFED driver. We download the MLNX_OFED_LINUX-4.5-1.0.1.0-rhel7.6-x86_64, and install it against our newly built kernel:

  <code># Install the same MLNX_OFED on both the CPU server and memory servers<br># Please refer the Mellanox website for more details.<br>cd MLNX_OFED_LINUX-4.5-1.0.1.0-rhel7.6-x86_64<br> sudo mlnxofedinstall --add-kernel-support</code>

  After installing the OFED driver, please confirm the RDMA works well between the CPU server and memory servers.

- Build and install *Semeru* RDMA module

  <code> \# Add the IP of each memory server into<br># *Semeru/linux-4.11-rc8/include/linux/swap_global_struct.h*<br> \# e.g., the Inﬁniband IPs of the 2 memory servers are *10.0.0.2* and *10.0.0.4*<br>char * mem_server_ip\[][] = {"10.0.0.2","10.0.0.4"}; <br> uint16_t mem_server_port = 9400;<br><br>\# Then build the Semeru RDMA module<br>make</code>



#### Install the CPU-Server JVM

We next discuss the steps to build and install the CPU-server JVM.

- Download Oracle JDK 12 to build Semeru JVM

  <code># Assume jdk 12.02 is under path, ${home_dir}/jdk12.0.2<br># Or change the path in shell script, Semeru/CPU-Server/build_cpu_server.sh<br>boot_jdk="${home_dir}/jdk12.0.2" </code>

- Build the CPU-server JVM

  <code># ${build_mode} can be one of the three modes:<br># slowdebug, fastdebug, or release.<br># We recommend fastdebug to debug the JVM code <br># and release to test the performance.<br># Please make sure both the CPU server and <br># memory servers use the same build mode.<br>cd Semeru/CPU-Server/<br>./build_cpu_server.sh ${build_mode}<br>./build_cpu_server.sh build<br># Take release mode as example — the compiled<br># JVM will be in:<br># Semeru/CPU-Server/build/linux-x86_64-server-release/jdk </code>

#### Install the Memory-Server LJVM

The next step is to install the LJVM on each memory server.

- Download OpenJDK 12 and build the LJVM

  <code># Assume OpenJDK12 is under the path<br>#${home_dir}/jdk-12.0.2<br># Or you can change the path in the script<br># Semeru/Memory-Server/build_mem_server.sh<br>boot_jdk="${home_dir}/jdk-12.0.2"</code>

- Change the IP addresses

  <code># E.g., mem-server #0’s IP is 10.0.0.2, ID is 0.<br># Change the IP address and ID in ﬁle:<br># Semeru/Memory-Server/src/hotspot/share/<br># utilities/globalDefinitions.hpp<br>#@Mem-server #0<br><br>#define NUM_OF_MEMORY_SERVER 1<br>#define CUR_MEMORY_SERVER_ID 0<br>static const char cur_mem_server_ip[] = "10.0.0.2";<br>static const char cur_mem_server_port[]= "9400";</code>

- Build and install the LJVM

  <code># Use the same ${build_mode} as the CPU-server JVM.<br>cd Semeru/Memory-Server/<br>./build_memory_server.sh ${build_mode}<br>./build_memory_server.sh build<br>./build_memory_server.sh install<br><br># The compiled Java home will be installed under:<br># {home_dir}/jdk12u-self-build/jvm/openjdk-12.0.2-internal<br># Set JAVA_HOME to point to this folder.</code>

#### Running Applications

To run applications, we ﬁrst need to connect the CPU server with memory servers. Next, we mount the remote memory pools as a swap partition on the CPU server. When the application uses more memory than the limit set by *cgroup*, its data will be swapped out to the remote memory via RDMA.

- Launch memory servers

  <code># Use the shell script to run each memory server.<br># ${execution_mode} can be *execution* or *gdb*.<br># @Each memory server<br>cd Semeru/ShellScrip<br>run_rmem_server_with_rdma_service.sh Case1 ${execution_mode}</code>

- Connect the CPU server with memory servers

  <code># @CPU server><br># The default size of remote memory server is 32GB:<br># 4GB meta region and 32GB data regions.<br># If not, assign the data regions size to the parameter <br># in Semeru/ShellScript/install_semeru_module.sh :<br># SWAP_PARTITION_SIZE="32G"<br> cd Semeru/ShellScript/<br>install_semeru_module.sh semeru<br><br># To close the swap partition, do the following:<br># @CPU server<br>cd Semeru/ShellScript/<br>install_semeru_module.sh close_semeru<br><br># However, caused of the shortcomings of *frontswap*,<br># we can't remove the registered frontswap device by doing this.<br># We have to restart the cpu server and re-connct the CPU server and memory servers.</code>

- Set a CPU server cache size limit for an application

  <code># E.g., Create a cgroup with 10GB memory limitation.<br># The shellscript will create a *cgroup*, named as *memctl*.<br># @CPU server<br>cd Semeru/ShellScript<br>cgroupv1_manage.sh create 10g</code>

- Add a Spark executor into the created *cgroup*

  <code># Add a Spark worker into the *cgroup*, *memctl*.<br># Its sub-process, executor, falls into the same *cgroup*.<br># Modify the function *start_instance* under<br># Spark/sbin/start-slave.sh<br># @CPU server<br>cgexec -sticky -g memory:memctl "${SPARK_HOME}/sbin" /sparkdaemon.sh start $CLASS $WORKER_NUM -webui-port "$WEBUI_PORT" $PORT_FLAG $PORT_NUM $MASTER "$@"<br><br># We also recommend that only run the executor on the CPU-Server JVM.<br># In order to achive this, specify the executor JVM in Spark/conf/spark-defaults.conf :<br>spark.executorEnv.JAVA_HOME=${semeru_cpu_server_jvm_dir}/jdk</code>

- Launch a Spark application

  Some *Semeru* JVM options need to be added for both CPU-server JVM and LVJMs. CPU-server JVM and memory server LJVMs should use the value for the same JVM option.

  <code># E.g., under the conﬁguration of 25% local memmory<br># 512MB Java heap Region<br># @CPU server<br>-XX:+SemeruEnableMemPool -XX:EnableBitmap -XX:-UseCompressedOops -Xnoclassgc -XX:G1HeapRegionSize=512M -XX:MetaspaceSize=0x10000000 -XX:SemeruLocalCachePercent=25<br><br># @Each memory server<br># ${MemSize}: the memory size of current memory server<br># ${ConcThread}: the number of concurrent threads<br>-XX:SemeruEnableMemPool -XX:-UseCompressedOops -XX:SemeruMemPoolMaxSize=${MemSize} -XX:SemeruMemPoolInitialSize=${MemSize} -XX:SemeruConcGCThreads=${ConcThread}<br><br># We provide some shellscript examples for Spark applications under the directory: Semeru/ShellScrip/SparkAppShellScrip<br># Please check their JVM parameters.</code>



# FAQ

*Semeru* is an academic proterotype to show the benefits of managing data on disaggreagted datacenters cross-layers. It does have some limitations and  we will keep updating the code of *Semeru*. If you encounter any problems, please open an issue or feel free to contact us: <br>Chenxi Wang *wangchenxi@cs.ucla.edu*; <br>Haoran Ma *haoranma@cs.ucla.edu*.



#### 1. How many JVMs can run on the CPU server ?

At this moment, only one JVM can run on the CPU server. When launch the Spark cluster, multiple daemon processes run in the backgroud, e.g., Worker, Executors. Each process is a separate JVM proces. The Worker process is used for management and the  Executor process is used for real computation. Please only let the Executor process runs on *Semeru* CPU server JVM. One Executor per CPU server. 

#### 2. Which part of data can be swapped out to memory servers ?

Part of the Meta region and all the Data regions can be swapped out to memory servers via the data path. Some meta  variables can't be swapped out to memory servers, even they are used as the purpose on both the CPU server and memory servers. Because these variables  contain virtual functions. For the same object instance, the virtual functions' addresses are usually different in the CPU server and memory servers. Swap out these objects instances from CPU server to memory servers are not safe.



# Know Issues

We have found some unfixed issues. Some of them are potential optimizations that can be applied. Some of them are potential bugs.  We will fix them and update the code latter.

#### 1. Using too much native memory can cause Out-Of-Memory error.

In our design, only part of the Meta space and the Data space (Java Heap) can be swapped out to memory servers. If the Java application uses too much native memory which exceeds the CPU server local capacity, the process will be killed by the Out-Of-Memory error. We will add a dedicated remote memory pool on the memory servers for the native memory space later.

#### 2. Some meta data in CPU server JVM can be freed. 

In order to do concurrent tracing, G1 GC maintains some large data structures, e.g., the bitmap. Its size can reach up to 1/32 of the Java heap size. *Semeru* moved all the concurrent tracing to memory servers. There is no need to keep these meta data structures. Removing them can save some time and space overhead.

#### 3. Java heap size is fixed at 32GB, Start at 0x400,100,000,000.

Some meta variables are related to the Java heap size. E.g., the CPU server swap file/partition size, memory servers' alive_bitmap size etc. It's a little hard to change the Java heap size right now. We will update a new version to fix this problem later.
