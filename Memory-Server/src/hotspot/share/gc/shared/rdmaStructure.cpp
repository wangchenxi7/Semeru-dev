/**
 * for RDMA communication structure.
 *  
 */

#include "gc/shared/rdmaStructure.hpp"   // why can't find this header by using shared/rdmaStructure.hpp


// The size of the flexible array _region_cset[] is limited by global macro, utilities/globalDefinitions.hpp :
//	#define MEMORY_SERVER_CSET_OFFSET     (size_t)0x8000000   // +128MB
//	#define MEMORY_SERVER_CSET_SIZE       (size_t)0x1000      // 4KB 
received_memory_server_cset::received_memory_server_cset():
_num_regions(0)
//_region_cset(NULL)		// Can't initialize the flexible array
{

	// debug
	#ifdef ASSERT
		tty->print("%s, invoke the constructor successfully.\n", __func__);
		tty->print("%s, addr of current instance : 0x%lx ,	addr of field _num_regions : 0x%lx,	addr of field _region_cset : 0x%lx \n", __func__,
																				(size_t)this, (size_t)&(this->_num_regions),
																				(size_t)this->_region_cset	);
	#endif
}



flags_of_cpu_server_state::flags_of_cpu_server_state():
_is_cpu_server_in_stw(false)
{
	
	// debug
	#ifdef ASSERT
		tty->print("%s, invoke the constructor successfully.\n", __func__);
		tty->print("%s, start address of current instance : 0x%lx , address of field _is_cpu_server_in_stw : 0x%lx \n", __func__,
																															(size_t)this, (size_t)&(this->_is_cpu_server_in_stw)	);
	#endif
}