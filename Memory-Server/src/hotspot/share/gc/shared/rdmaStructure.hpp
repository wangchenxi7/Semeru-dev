/**
 * This file contains all the class or structure used by RDMA transferring.
 *  
 */
#include "utilities/align.hpp"
#include "utilities/sizes.hpp"
#include "utilities/globalDefinitions.hpp"
#include "memory/allocation.inline.hpp"


#ifndef SHARE_GC_SHARED_RDMA_STRUCTURE
#define SHARE_GC_SHARED_RDMA_STRUCTURE

#define RDMA_STRUCTURE_ALIGNMENT			16

#define MEM_SERVER_CSET_BUFFER_SIZE		(size_t)(512 - 8) 	


/**
 * Limit the instance size within 4KB 
 * 
 * size_t, unsigned long, 8 bytes.
 * 
 * 1) CPU server send data to rewrite the value of _num_regions and _regions_cset[].
 * 		This instance has to be allocated in a fixed memory range, reserved by both CPU server and memory server.
 * 2) Use _num_regions as flag of if new data are written here by CPU server, producer.
 * 		Also use _num_regions to index the content of _region_cset[], decreased by Memory server, consumer.
 * 3) This function is not MT safe. We ausme that only one thread can invoke this function.
 * 
 */
class received_memory_server_cset : public CHeapRDMAObj<received_memory_server_cset>{

private :
	// First field, identify if CPU server pushed new Regions here.
	size_t 	_num_regions;

	// [?] a flexible array, points the memory just behind this instance.
	// The size of current instance should be limited within 4K, 
	// The array size should be limited by MEM_SERVER_CSET_BUFFER_SIZE.
	size_t	_region_cset[];			



public :
	
	received_memory_server_cset();
	
	size_t*	num_received_regions()	{	return &_num_regions;	}


	// Allocate the _region_cset just behind this instance ?
	// void initialize(){
	// 	_num_regions = 0;
	// }



	void reset()	{	_num_regions = 0;	}

	//
	// This function isn't MT safe.
	// 
	int	pop()	{
		if(_num_regions >= 1)
			return _region_cset[--_num_regions];	
		else
			return -1; 
	}

};


/**
 * Memory server need to know the current state of CPU srever to make its own decesion.
 * For example, if the CPU server is in STW GC now, memory server switch to Remark and Compact cm_scanned Regions.
 * Or memory server keeps concurrently tracing the freshly evicted Regions.
 *  
 * 	These flags are only writable by CPU server.
 *  Memory server only read the value of them.
 *  These varialbes are valotile, every time Memory server needs to read the value from memory.
 * 
 */
class flags_of_cpu_server_state : public CHeapRDMAObj<flags_of_cpu_server_state>{
	private :
		volatile bool _is_cpu_server_in_stw;

	public :
		flags_of_cpu_server_state();

		inline bool is_cpu_server_in_stw()	{	return _is_cpu_server_in_stw;	}

};


#endif // SHARE_GC_SHARED_RDMA_STRUCTURE