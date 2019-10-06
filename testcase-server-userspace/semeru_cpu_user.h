/**
 * User space level CPU server RDMA client. 
 * 
 * The ways of building RDMA connections are all changed.
 * 
 */

#ifndef REMOTEMEMPOOL
#define REMOTEMEMPOOL



//
// User space headers
//
#include <stdio.h>
#include <stdlib.h>

// For infiniband
#include <rdma/rdma_cma.h>
#include <arpa/inet.h>
#include <netinet/in.h>

// Utilities 
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <semaphore.h>
#include <errno.h>



//
// Disk hardware information
//
#define RMEM_PHY_SECT_SIZE					512 	// physical sector seize, used by driver (to disk).
#define RMEM_LOGICAL_SECT_SIZE			512	// logical sector seize, used by kernel (to i/o).
//#define RMEM_REQUEST_QUEUE_NUM     	2  		// for debug, use the online_cores
#define RMEM_QUEUE_DEPTH           	16  	// [?]  1 - (-1U), what's the good value ? 
#define RMEM_QUEUE_MAX_SECT_SIZE		1024 	// The max number of sectors per request, /sys/block/sda/queue/max_hw_sectors_kb is 256
#define DEVICE_NAME_LEN							32



//
// RDMA related macros  
//
#define PAGE_SIZE						4096
#define ONE_GB							1024*1024*1024	// byte size of ONE GB
#define CHUNK_SIZE_GB									1		// Must be a number of 2 to power N.
#define MAX_REMOTE_MEMORY_SIZE_GB 		32 		// The max remote memory size of current client. For 1 remote memory server, this value eaquals to the remote memory size.
#define RDMA_READ_WRITE_QUEUE_DEPTH		16		// [?] connection with the Disk dispatch queue depth ??
extern uint64_t RMEM_SIZE_IN_PHY_SECT;

// 1-sieded RDMA Macros

// Each request can have multiple bio, but each bio can only have 1  pages ??
#define MAX_REQUEST_SGL								64 		// !! Not sure about this, debug.  
#define ONE_SIEDED_RDMA_BUF_SIZE			MAX_REQUEST_SGL * PAGE_SIZE


//Debug
#define DEBUG_RDMA_CLINET 1


/**
 * ################### utility functions ####################
 */

//
// Calculate the number's power of 2.
// [?] can we use the MACRO ilog2() ?
// uint64_t power_of_2(uint64_t  num){
    
//     uint64_t  power = 0;
//     while( (num = (num >> 1)) !=0 ){
//         power++;
//     }

//     return power;
// }




/**
 * Bit operations 
 * 
 */
#define GB_SHIFT 			30 
#define CHUNK_SHIFT			(GB_SHIFT + ilog2(CHUNK_SIZE_GB))	 // Used to calculate the chunk index in Client (File chunk). Initialize it before using.
#define	CHUNK_MASK			((1 << CHUNK_SHIFT)-1)

#define RMEM_LOGICAL_SECT_SHIFT		(ilog2(RMEM_LOGICAL_SECT_SIZE))  // the power to 2, shift bits.

//
// File address to Remote virtual memory address translation
//


//
// Enable debug information printing 
//
#define DEBUG_RDMA_CLIENT 1 
//#define DEBUG_RDMA_CLINET_DETAIL 1
//#define DEBUG_BD_RDMA_SEPARATELY 1			// Build and install BD & RDMA modules, but not connect them.
//#define DEBUG_RDMA_ONLY		   1			// Only build and install RDMA modules.

// from kernel 
/*  host to network long long
 *  endian dependent
 *  http://www.bruceblinn.com/linuxinfo/ByteOrder.html
 */
#define ntohll(x) (((uint64_t)(ntohl((int)((x << 32) >> 32))) << 32) | \
		    (unsigned int)ntohl(((int)(x >> 32))))
#define htonll(x) ntohll(x)

#define htonll2(x) cpu_to_be64((x))
#define ntohll2(x) cpu_to_be64((x))



//
// ################################ Structure definition of RDMA ###################################
//
// Should initialize these RDMA structure before Block Device structures.
//



/**
 * Used for message passing control
 * For both CM event, data evetn.
 * RDMA data transfer is desinged in an asynchronous style. 
 */
enum rdma_session_context_state { 
	IDLE = 1,		 // 1, Start from 1.
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,		// 5,  updated by IS_cma_event_handler()

	FREE_MEM_RECV,		// Received free memory information from Memory server.
	RECEIVED_CHUNKS,	// get chunks from remote memory server
	RDMA_BUF_ADV,   // designed for server
	WAIT_OPS,
	RECV_STOP,    	// 10

	RECV_EVICT,
	RDMA_WRITE_RUNNING,
	RDMA_READ_RUNNING,
	SEND_DONE,
	RDMA_DONE,     	// 15

	RDMA_READ_ADV,	// updated by IS_cq_event_handler()
	RDMA_WRITE_ADV,
	CM_DISCONNECT,
	ERROR,
	TEST_DONE,		// 20, for debug
};



enum mem_type {
	DMA = 1,
	FASTREG = 2,
	MW = 3,
	MR = 4
};


	enum message_type{
		DONE = 1,				// Start from 1
		GOT_CHUNKS,				// Get the remote_addr/rkey of multiple Chunks
		GOT_SINGLE_CHUNK,		// Get the remote_addr/rkey of a single Chunk
		FREE_SIZE,
		EVICT,        			// 5
		ACTIVITY,				
		
		STOP,					//7, upper SIGNALs are used by server, below SIGNALs are used by client.

		REQUEST_CHUNKS,
		REQUEST_SINGLE_CHUNK,	// Send a request to ask for a single chunk.
		QUERY         			// 10
	};

/**
 * Two-sided RDMA message structure.
 * We use 2-sieded RDMA communication to exchange information between Client and Server.
 * Both Client and Server have the same message structure. 
 */
struct message {
  	
	// Information of the chunk to be mapped to remote memory server.
	uint64_t buf[MAX_REMOTE_MEMORY_SIZE_GB/CHUNK_SIZE_GB];		// Remote addr.
  uint32_t rkey[MAX_REMOTE_MEMORY_SIZE_GB/CHUNK_SIZE_GB];   	// remote key
  int size_gb;						// Size of the chunk ?

	enum message_type type;
};




/**
 * Need to update to Region status.
 */
enum chunk_mapping_state {
	EMPTY,		// 0
	MAPPED,		// 1, Cached ? 
};


enum region_status{
  NEWLY_ALLOCATED,			// 0, newly allocated ?
	CACHED,			// Partial or Fully cached Regions.
  EVICTED			// 2, clearly evicted to Memory Server
};


//
// Default size is CHUNK_SIZE_GB, 1 GB default.
//
struct remote_mapping_chunk {
	uint32_t 					remote_rkey;		/* RKEY of the remote mapped chunk */
	uint64_t 					remote_addr;		/* Virtual address of remote mapped chunk */
	enum chunk_mapping_state 	chunk_state;
};



/**
 * Use the chunk as contiguous File chunks.
 * 
 * For example,
 * 	File address 0x0 to 0x3fff,ffff  is mapped to the first chunk, remote_mapping_chunk_list->remote_mapping_chunk[0].
 * 	When send 1 sided RDMA read/write, the remote address shoudl be remote_mapping_chunk_list->remote_mapping_chunk[0].remote_addr + [0 to 0x3fff,ffff]
 * 
 */
struct remote_mapping_chunk_list {
	struct remote_mapping_chunk *remote_chunk;		
	//uint32_t chunk_size_gb;  		// defined in macro
	uint32_t remote_free_size_gb;
	uint32_t chunk_num;				// length of remote_chunk list
	uint32_t chunk_ptr;				// points to first empty chunk. 

};







// This is the kernel level RDMA over IB.  
// Need to use the API defined in kernel headers. 
//



/**
 * 1-sided RDMA (read/write) message.
 * One rmem_rdma_command for each I/O requst.
 * 		i.e. Have registered RDMA buffer, used to send a wr to the QP.
 * 			[?] Can we optimize here ?? use the data in bio as DMA buffer directly.
 *  	i.e. a. request will reserve size for this request command : requet->cmd_size					
 * 			 	Acccess by : blk_mq_rq_to_pdu(request)
 * 			 b. ib_rdma_wr->ib_send_wr->wr_id also points to this context.
 * 
 * Fields
 * 		ib_sge		: Store the data to be sent to remote memory.
 * 		ib_rdma_wr 	: The WR used for 1-sided RDMA read/write.
 * 		request		: Transfer this requste to 1 sided RDMA message.
 * 						And then resue this request as reponds I/O requseet.
 * 						Because this request has the same request->tag which is used for monitoring if a sync request is finished.
 * 
 * More Explanation
 *	Build a seperate WR for each I/O request and sent them to remote memory pool via  RDMA read/write.
 * 
 */
struct rmem_rdma_command{

//	struct IS_connection 	*IS_conn;
//	struct free_ctx_pool 	*free_ctxs;  	// points to its upper structure, free_ctx_pool.
	//struct mutex ctx_lock;					// Does a single rmem_rdma_cmd need a spin lock ??
 
	struct ibv_send_wr 	rdma_sq_wr;			// wr for RDMA write/send. use send_wr as 1-sided RDMA read/write.
	struct ibv_sge 			rdma_sgl;				// Points to the data addres
	char 								*rdma_buf;			// The reserved DMA buffer. Binded to ib_sge. [?] the size of the buffer isn't determined before get the i/o request?
	//uint32_t 				buffer_len;				// PAGE_SIZE*MAX_SGL_LENGTH
	//dma_addr_t  		rdma_dma_addr;		// DMA/BUS address of rdma_buf
	//uint64_t								rdma_dma_addr;		// After confirm the IB is 64 bits compatible, use the dma_addr_t.
	//DECLARE_PCI_UNMAP_ADDR(rdma_mapping)	// [?]
	struct ibv_mr 		*rdma_mr;					// The memory region. No one use this field. Use the rdma_buf directly.
	
	//
	// I/O request file address
	//
	//struct request 		*io_rq;			// Pointer to the I/O requset. [!!] Contain Real Offload [!!]

	//
	// Can we delete these fields ?
	//
	//int 										chunk_index;	// Hight 32 bits, the chunk index (Assume 1GB/Chunk)
	//unsigned long 					offset;			// Offset within the chunk
	//unsigned long 					len;			// Length of the i/o reuqet data, page alignment.
//	struct remote_chunk_g 			*chunk_ptr;
	//atomic_t 						free_state; 	//	available = 1, occupied = 0
	int 							free_state;

	//struct rmem_device_control 		*rmem_dev_ctx;		// disk driver_data, get from rdma_session-> rmem_dev_ctx;
	//struct rdma_session_context		*rdma_session;	// RDMA connection context. We use a global rdma_session_global now.
};





/**
 * Assign a rmem_rdma_queue to each dispatch queue to handle the poped request.
 * 		 rmem_rdma_queue is added to	 blk_mq_hw_ctx->driver_data
 * 
 * 
 * Fields
 * 		struct rmem_rdma_command* rmem_rdma_cmd,  a list of rmem_rdma_command. 
 * 			Use a single pointer to traverse the rmem_rdma_cmd_list.
 * 			list_head : rmem_rdma_cmd_list
 * 			list_tail : rmem_rdma_cmd_list + rmem_rdma_queue->length -1
 * 
 * 		struct rmem_device_control, points to Disk Driver controller/context.
 * 											
 * 			
 * More Explanation
 * 	[?] But all these rdma_queues race for the same ib_qp ?? 
 * 	All the RDMA read/write WR are attached to the only ib_qp ??
 *  
 * 	Each poped I/O request needs a rmem_rdma_command to handle.
 * 	Length of rmem_rdma_cmd_list should match the depth/length of dispatch queue.
 * 	The rmem_rdma_cmd_list should be allocated intialized in advance.
 * 
 * 
 */
struct rmem_rdma_queue {
	struct rmem_rdma_command		*rmem_rdma_cmd_list;	// one-sided RDMA read/write message list, one for each i/o request
	uint32_t 										rdma_queue_depth;		// length of the rmem_rdma_comannd_list.
	uint32_t										cmd_ptr;				// Manage the rmem_rdma_cmd_list in a bump pointer way.
	uint32_t										q_index;				// Assign the hw_index in blk_mq_ops->.init_hctx.

	//struct rmem_device_control		*rmem_dev_ctrl;  		// point to the device driver/context
	struct rdma_session_context		*rdma_session;			// Record the RDMA session this queue belongs to.
	// other fields

	pthread_spinlock_t rdma_queue_lock;		// Used for manipulating on the rmem_rdma_cmd_list

};




/**
 * Mange the RDMA connection to a remote server. 
 * Every Remote Memory Server has a dedicated rdma_session_context as controller.
 * 	
 * Fields
 * 		rdma_cm_id		:  the RDMA device ?
 * 		Queue Pair (QP) : 
 * 		Completion Queue (CQ) :
 * 		
 */
struct rdma_session_context {

	// cm events
	struct rdma_event_channel		*event_channel;
	struct rdma_conn_param			conn_param;
	struct rdma_cm_id 					*cm_id;	// IB device information ?
	struct rdma_cm_event				*event;  	// a  global event pointer ?

  // ib events 
	struct ibv_comp_channel			*comp_channel; 
  struct ibv_cq *cq;			// Both send/recieve queue share the same ib_cq.
	struct ibv_pd *pd;
	struct ibv_qp *qp;			//[!] There is only one QP for a Remote Memory Server.


  // For infiniband connection rdma_cm operation
  uint16_t 	port;					/* dst port in NBO */
	uint8_t 	addr[16];			// 	4 bytes, 32 bits.	
  uint8_t 	addr_type;		/* AF_INET - IPv4 */
  int 			txdepth;			/* SQ depth */  // [?] receive and send queue depth  use this same depth ?? CQ entry depth x2 +1??

  enum rdma_session_context_state 	state;		/* used for cond/signalling */
  sem_t 								sem;      	// semaphore for wait/wakeup
	uint8_t  	freed;			// some function can only be called once, this is the flag to record this.

	//
  	// 3) 2-sided RDMA section. 
  	//		This section is used for RDMA connection and basic information exchange with remote memory server.

  	// DMA Receive buffer
  struct ibv_recv_wr 	rq_wr;			// receive queue wr
	struct ibv_sge 			recv_sgl;		/* recv single SGE */
  struct message			*recv_buf;
  	//dma_addr_t 			recv_dma_addr;	// It's better to check the IB DMA address limitations, 32 or 64
	//uint64_t									recv_dma_addr;	
	//DECLARE_PCI_UNMAP_ADDR(recv_mapping)	// Use MACRO to define a DMA fields. It will do architrecture check first. same some fields.
	struct ibv_mr 			*recv_mr;

  	// DMA send buffer
	// ib_send_wr, used for posting a two-sided RDMA message 
  struct ibv_send_wr 	sq_wr;			// send queue wr
	struct ibv_sge 		send_sgl;
	struct message		*send_buf;
	//dma_addr_t 			send_dma_addr;
//	uint64_t 							send_dma_addr;
	//DECLARE_PCI_UNMAP_ADDR(send_mapping)
	struct ibv_mr 		*send_mr;


	// [?]Unknown porpuse ??
	// get lkey from dma_mr->lkey.
	struct ibv_mr 		*dma_mr;  // [?] receive mr to be registered ??
  	enum mem_type 		mem;		//  only when mem == DMA, we need allocate and intialize dma_mr.
	    // [?] How to set this value ?
	//int 				local_dma_lkey;		/* use 0 for lkey */  //[??] No one touch this value, it keeps 0.



	// 4) For 1-sided RDMA read/write
	// Record the mapped chunk
	// Should we try to map all the chunk at the start time ?

//	struct ib_mr *rdma_mr;

//  	uint64_t remote_len;		/* remote guys LEN */
	// Record the mapped chunk information.
	//

	// I/O request --> 1 sided RDMA message.
	// rmem_rdma_queue store these reserved 1-sided RDMA wr information.
	struct rmem_rdma_queue		*rmem_rdma_queue_list; 		// one rdma_queue per dispatch queue.


	// 5) manage the CHUNK mapping.
	struct remote_mapping_chunk_list remote_chunk_list;




	//
	// Points to the Disk Driver Controller/Context.
	// RDMA part is initialized first, 
	// then invoke "RMEM_init_disk_driver(void)" to get the Disk Driver controller.
	//
	//struct rmem_device_control		*rmem_dev_ctrl;

};




struct request{

};

struct rmem_device_control{

};


//
// ================================= Block Device Part =============================
//






















/**
 * ########## Function declaration ##########
 * 
 * static : static function in C means that this function is only callable in this file.
 * 
 */



int 	octopus_RDMA_connect(struct rdma_session_context *rdma_session_ptr);
//int 	octopus_rdma_cm_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event);
int 	octopus_rdma_cm_event_handler(struct rdma_session_context *rdma_session_ptr);

//void 	octopus_cq_event_handler(struct ibv_cq * cq, void *rdma_session_context);
int 	octopus_cq_event_handler(struct rdma_session_context 	*rdma_session_ptr);
int 	handle_recv_wr(struct rdma_session_context *rdma_session, struct ibv_wc *wc);
int 	send_message_to_remote(struct rdma_session_context *rdma_session, int messge_type  , int size_gb);
void 	map_single_remote_memory_chunk(struct rdma_session_context *rdma_session_ptr);


// 1-sided RDMA message
int		post_rdma_write(struct rdma_session_context *rdma_session_ptr, struct request* io_rq, struct rmem_rdma_queue* rdma_q_ptr,  
					struct remote_mapping_chunk *remote_chunk_ptr, uint64_t offse_within_chunk, uint64_t len );
int 	rdma_write_done(struct ibv_wc *wc);

int 	post_rdma_read(struct rdma_session_context *rdma_session_ptr, struct request* io_rq, struct rmem_rdma_queue* rdma_q_ptr,  
					struct remote_mapping_chunk *remote_chunk_ptr, uint64_t offse_within_chunk, uint64_t len );
int 	rdma_read_done(struct ibv_wc *wc);

inline void free_a_rdma_cmd_to_rdma_q(struct rmem_rdma_command* rdma_cmd_ptr);
struct rmem_rdma_command* get_a_free_rdma_cmd_from_rdma_q(struct rmem_rdma_queue* rmda_q_ptr);


// Chunk management
int 	init_remote_chunk_list(struct rdma_session_context *rdma_session_ptr );
void 	bind_remote_memory_chunks(struct rdma_session_context *rdma_session_ptr );



// Transfer Block I/O to RDMA message
int 	transfer_requet_to_rdma_message(struct rmem_rdma_queue* rdma_q_ptr, struct request * rq);
void 	copy_data_to_rdma_buf(struct request *io_rq, struct rmem_rdma_command *rdma_ptr);


//
// Block Device functions
//
int   rmem_init_disk_driver(struct rmem_device_control *rmem_dev_ctl);
int 	RMEM_create_device(char* dev_name, struct rmem_device_control* rmem_dev_ctrl );

//
// Resource free
//
void 	octopus_free_buffers(struct rdma_session_context *rdma_session);
void 	octopus_free_rdma_structure(struct rdma_session_context *rdma_session);
int 	octopus_disconenct_and_collect_resource(struct rdma_session_context *rdma_session);

int 	octopus_free_block_devicce(struct rmem_device_control * rmem_dev_ctl );



//
// Debug functions
//
char* rdma_message_print(int message_id);
char* rdma_session_context_state_print(int id);
char* rdma_cm_message_print(int cm_message_id);
char* rdma_wc_status_name(int wc_status_id);

/** 
 * ########## Declare some global varibles ##########
 * 
 * There are 2 parts, RDMA parts and Disk Driver parts.
 * Some functions are stateless and they are only used for allocate and initialize the variables.
 * So, we need to keep some global variables and not not exit the Main function.
 * 
 * 	1). Data allocated by kzalloc, kmalloc etc. will not be freed by the kernel.  
 *  	1.1) If we don't want to use any data, we need to free them mannually.	
 * 		1.2) Fileds allocated by kzalloc and initiazed in the stateless functions will stay available.
 * 
 *  2) Do NOT define global variables in header, only declare extern variables in header and implement them in .c file.
 */

// Initialize in main().
// One rdma_session_context per memory server connected by IB.
extern struct rdma_session_context 	rdma_session_global;   // [!!] Unify the RDMA context and Disk Driver context global var [!!]
extern struct rmem_device_control  	rmem_dev_ctrl_global;



extern int rmem_major_num;
extern int online_cores;		// Both dispatch queues, rdma_queues equal to this online_cores.


//debug
extern uint64_t	rmda_ops_count;
extern uint64_t	cq_notify_count;
extern uint64_t	cq_get_count;







#endif // REMOTEMEMPOOL


