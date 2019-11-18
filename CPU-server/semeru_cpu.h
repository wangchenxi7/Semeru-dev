/**
 * Register a Remote Block Device under current OS.
 * 
 * The connection is based on RDMA over InfiniBand.
 * 
 */

#ifndef REMOTEMEMPOOL
#define REMOTEMEMPOOL

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h> 
#include <linux/init.h>

// Block layer 
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/swapfile.h>
#include <linux/swap.h>

// For infiniband
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/pci-dma.h>
#include <linux/pci.h>			// Use the dma_addr_t defined in types.h as the DMA/BUS address.
#include <linux/inet.h>
#include <linux/lightnvm.h>
#include <linux/sed-opal.h>

// Utilities 
#include <linux/log2.h>
#include<linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/scatterlist.h>



//
// Disk hardware information
//

// According to the implementation of kernle, it aleays assume the (logical ?) sector size to be 512 bytes.
#define RMEM_PHY_SECT_SIZE					(u64)512 	// physical sector seize, used by driver (to disk).
#define RMEM_LOGICAL_SECT_SIZE			(u64)512		// logical sector seize, used by kernel (to i/o).

#define RMEM_QUEUE_DEPTH           	(u64)16  	// [?]  1 - (-1U), what's the good value ? 
#define RMEM_QUEUE_MAX_SECT_SIZE		(u64)1024 	// The max number of sectors per request, /sys/block/sda/queue/max_hw_sectors_kb is 256
#define DEVICE_NAME_LEN							(u64)32



//
// RDMA related macros  
//
#define ONE_GB												((u64)1024*1024*1024)	// byte size of ONE GB, is this safe ? exceed the int.
#define CHUNK_SIZE_GB									(u64)8			// Must be a number of 2 to power N.
#define MAX_REMOTE_MEMORY_SIZE_GB 		(u64)32 		// The max remote memory size of current client. For 1 remote memory server, this value eaquals to the remote memory size.
#define RDMA_READ_WRITE_QUEUE_DEPTH		(u64)16			// [?] connection with the Disk dispatch queue depth ??
extern u64 RMEM_SIZE_IN_PHY_SECT;

// 1-sieded RDMA Macros

// Each request can have multiple bio, but each bio can only have 1  pages ??
#define MAX_REQUEST_SGL								 32 		// number of segments, get from ibv_query_device.
#define MAX_SEGMENT_IN_REQUEST				(u64)128 // defined in ? copy from nvmeof's SG_CHUNK_SIZE
#define ONE_SIEDED_RDMA_BUF_SIZE			(u64)MAX_REQUEST_SGL * PAGE_SIZE





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
#define	CHUNK_MASK			(u64)((1UL << CHUNK_SHIFT)-1)

#define RMEM_LOGICAL_SECT_SHIFT		(ilog2(RMEM_LOGICAL_SECT_SIZE))  // the power to 2, shift bits.

//
// File address to Remote virtual memory address translation
//


//
// Enable debug information printing 
//
// #define DEBUG_RDMA_CLIENT 			1 
#define DEBUG_LATENCY_CLIENT		1
//#define DEBUG_RDMA_CLIENT_DETAIL 1
//#define DEBUG_BD_ONLY 1			// Build and install BD & RDMA modules, but not connect them.
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

	FREE_MEM_RECV,
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
 
	struct ib_rdma_wr 	rdma_sq_wr;			// wr for RDMA write/send.  [?] Send queue wr ??
	
	//
	// I/O request file address
	//
	struct request 			*io_rq;			// Pointer to the I/O requset. [!!] Contain Real Offload [!!]

		
	struct ib_sge sge_list[MAX_REQUEST_SGL]; // scatter-gather entry for 1-sided RDMA read/write WR. 

	// fields for debug
	#ifdef DEBUG_LATENCY_CLIENT
		struct rmem_rdma_queue *rdma_q_ptr;		// points to the rdma queue it belongs to.
	#endif

	// scatterlist  - this works like sge_table.
	// [!!] The scatterlist is temporaty data, we can put in the behind of i/o request and not send them to remote server.
	// points to the physical pages of i/o requset. 
	u64  								nentry;		// number of the segments, usually one pysical page per segment
	struct scatterlist	sgl[];		// Just keep a pointer. The memory is reserved behind the i/o request. 
	// scatterlist should be at the end of this structure : Variable/Flexible array in structure.
	// Then it can points to reserved space in i/o requst  after sizeof(struct rmem_rdma_command).
};





/**
 * Assign a rmem_rdma_queue to each dispatch queue to handle the poped request.
 * 		 rmem_rdma_queue is added to	 blk_mq_hw_ctx->driver_data
 * 
 * [?] We removed the rmem_rdma_command pool. Then is it necessary to keep this rdma_queue ??
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
	struct rmem_rdma_command			*rmem_rdma_cmd_list;	// one-sided RDMA read/write message list, one for each i/o request
	uint32_t 											rdma_queue_depth;		// length of the rmem_rdma_comannd_list.
	uint32_t											cmd_ptr;						// Manage the rmem_rdma_cmd_list in a bump pointer way.
	uint32_t											q_index;						// Assign the hw_index in blk_mq_ops->.init_hctx.

	//struct rmem_device_control		*rmem_dev_ctrl;  		// point to the device driver/context
	struct rdma_session_context		*rdma_session;			// Record the RDMA session this queue belongs to.
	// other fields

	spinlock_t rdma_queue_lock;		// Used for manipulating on the rmem_rdma_cmd_list

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
 * More explanation 
 * 	In our design, the remote memory driver can have several RDMA sessions connected to
 * 	differnt Memory server. All the JVM heap constitue the universal Java heap.
 * 
 * [?] how can we get the device for MR register??
 * 
 */
struct rdma_session_context {

	// cm events
	struct rdma_cm_id *cm_id;	// IB device information ?

  // ib events 
  struct ib_cq *cq;			// Both send/recieve queue share the same ib_cq.
	struct ib_pd *pd;
	struct ib_qp *qp;			//[!] There is only one QP for a Remote Memory Server.


  // For infiniband connection rdma_cm operation
  uint16_t 	port;			/* dst port in NBO */
	u8 				addr[16];		/* dst addr in NBO */
  uint8_t 	addr_type;		/* ADDR_FAMILY - IPv4/V6 */
  int 			txdepth;		/* SQ depth */  // [?] receive and send queue depth  use this same depth ?? CQ entry depth x2 +1??

  enum rdma_session_context_state 	state;		/* used for cond/signalling */
  wait_queue_head_t 								sem;      	// semaphore for wait/wakeup
	uint8_t  	freed;			// some function can only be called once, this is the flag to record this.

	//
  // 3) 2-sided RDMA section. 
  //		This section is used for RDMA connection and basic information exchange with remote memory server.

  // DMA Receive buffer
  struct ib_recv_wr 	rq_wr;			// receive queue wr
	struct ib_sge 			recv_sgl;		/* recv single SGE */
  struct message			*recv_buf;
  	//dma_addr_t 			recv_dma_addr;	// It's better to check the IB DMA address limitations, 32 or 64
	u64									recv_dma_addr;	
	//DECLARE_PCI_UNMAP_ADDR(recv_mapping)	// Use MACRO to define a DMA fields. It will do architrecture check first. same some fields.
	//struct ib_mr *recv_mr;

  	// DMA send buffer
	// ib_send_wr, used for posting a two-sided RDMA message 
  struct ib_send_wr 	sq_wr;			// send queue wr
	struct ib_sge 			send_sgl;
	struct message			*send_buf;
	//dma_addr_t 			send_dma_addr;
	u64 								send_dma_addr;
	//DECLARE_PCI_UNMAP_ADDR(send_mapping)
	//struct ib_mr *send_mr;


	// [?]Unknown porpuse ??
	// get lkey from dma_mr->lkey.
	struct ib_mr 			*dma_mr;  // [?] receive mr to be registered ??
  enum mem_type 		mem;		//  only when mem == DMA, we need allocate and intialize dma_mr.
	

	// 4) For 1-sided RDMA read/write
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
	struct rmem_device_control		*rmem_dev_ctrl;

};









//
// ================================= Block Device Part =============================
//








/**
 * Block device information
 * This structure is the driver context data.  
 * 		i.e. blk_mq_tag_set->driver_data
 * 
 * 
 * [?] One rbd_device_control should record all the connection_queues ??
 * 
 * [?] Finnaly, we need pass all these fields to  tag_set,  htcx, rdma_connections ?
 * 
 * 
 */
struct rmem_device_control {
	//int			     		fd;   // [?] Why do we need a file handler ??
	int			     			major; /* major number from kernel */
	//struct r_stat64		     stbuf; /* remote file stats*/
	//char			     						file_name[DEVICE_NAME_LEN];       // [?] Do we need a file name ??
	//struct list_head	     		list;        /* next node in list of struct IS_file */    // Why do we need such a list ? only one Swap Partition.
	struct gendisk		    	*disk;			// The disk information, logical/physical sectior size ? 
	struct request_queue	    *queue; 		// Queue controller/context, controll both the staging queue and dispatch queue.
	struct blk_mq_tag_set	    tag_set;		// Used for information passing. Define blk_mq_ops.(block_device_operations is defined in gendisk.)
	
	unsigned int		      	queue_depth;    // Number of the on-the-fly request for each dispatch queue. 
	unsigned int		      	nr_queues;		// Number of dispatch queue, pass to blk_mq_tag_set->nr_hw_queues ??
	//int			            index; 							/* drive idx */
	char			            dev_name[DEVICE_NAME_LEN];
	//struct config_group	     dev_cg;
	spinlock_t		        	rmem_ctl_lock;					// mutual exclusion
	//enum IS_dev_state	     state;	

	//
	//struct bio_list bio_list;

	uint8_t  					freed; 			// initial is 0, Block resource is freed or not. 

	//
	// RDMA related information
	//
	//[?] What's  this queue used for ?
  	//struct rmem_rdma_queue	  	*rdma_queues;			//  [?] The rdma connection session ?? one rdma session queue per software staging queue ??
	struct rdma_session_context	*rdma_session;				// RDMA connection context, one per Remote Memory Server.


	// Below fields are used for debug.
	struct block_device *bdev_raw;			// Points to a local block device. 


};

















/**
 * ########## Function declaration ##########
 * 
 * static : static function in C means that this function is only callable in this file.
 * 
 */



int 	octopus_RDMA_connect(struct rdma_session_context *rdma_session_ptr);
int 	octopus_rdma_cm_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event);

void 	octopus_cq_event_handler(struct ib_cq * cq, void *rdma_session_context);
int 	handle_recv_wr(struct rdma_session_context *rdma_session, struct ib_wc *wc);
int 	send_message_to_remote(struct rdma_session_context *rdma_session, int messge_type  , int size_gb);
void 	map_single_remote_memory_chunk(struct rdma_session_context *rdma_session);


// 1-sided RDMA message

int build_rdma_wr(struct rmem_rdma_queue* rdma_q_ptr, struct rmem_rdma_command *rdma_cmd_ptr, struct request * io_rq, 
									struct remote_mapping_chunk *	remote_chunk_ptr , uint64_t offse_within_chunk, uint64_t len);

int		post_rdma_write(struct rdma_session_context *rdma_session, struct request* io_rq, struct rmem_rdma_queue* rdma_q_ptr,  
					struct remote_mapping_chunk *remote_chunk_ptr, uint64_t offse_within_chunk, uint64_t len );
int 	rdma_write_done(struct ib_wc *wc);

int 	post_rdma_read(struct rdma_session_context *rdma_session, struct request* io_rq, struct rmem_rdma_queue* rdma_q_ptr,  
					struct remote_mapping_chunk *remote_chunk_ptr, uint64_t offse_within_chunk, uint64_t len );
int 	rdma_read_done(struct ib_wc *wc);

inline void free_a_rdma_cmd_to_rdma_q(struct rmem_rdma_command* rdma_cmd_ptr);
struct rmem_rdma_command* get_a_free_rdma_cmd_from_rdma_q(struct rmem_rdma_queue* rmda_q_ptr);


// Chunk management
int 	init_remote_chunk_list(struct rdma_session_context *rdma_session );
void 	bind_remote_memory_chunks(struct rdma_session_context *rdma_session );



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

void 	print_io_request_physical_pages(struct request *io_rq, const char* message);
void 	print_scatterlist_info(struct scatterlist* sl_ptr , int nents );
void 	check_segment_address_of_request(struct request *io_rq);
bool 	check_sector_and_page_size(struct request *io_rq, const char* message);
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
extern u64	rmda_ops_count;
extern u64	cq_notify_count;
extern u64	cq_get_count;


#ifdef DEBUG_LATENCY_CLIENT
extern u32 * cycles_high_before; 
extern u32 * cycles_high_after;
extern u32 * cycles_low_before;
extern u32 * cycles_low_after;
#endif




#endif // REMOTEMEMPOOL


