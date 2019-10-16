/**
 * Works like a block device driver:
 * 
 * 1) Register a block device under /dev/xxx
 * 
 * 2) Define read/write i/o operation
 * 
 * 3) i/o queue
 * 
 * 4) maintain the RDMA connection to remote server.
 * 
 * 
 * ##########################################
 * The hierarchy :
 * 
 * File system    // [?] What's its purpose ?
 *     V
 *  I/O layer     // handle  the i/o request from file system.
 *     V
 *  Disk dirver   // We are working on this layer.
 * 
 * ###########################################
 * 
 */


// Self defined headers
#include "semeru_cpu.h"



//
// Define global variables
//

int rmem_major_num;
struct rmem_device_control      rmem_dev_ctrl_global;
u64 RMEM_SIZE_IN_PHY_SECT =     ONE_GB / RMEM_PHY_SECT_SIZE * MAX_REMOTE_MEMORY_SIZE_GB;    // 16M physical sector, 8GB.

#ifdef DEBUG_LATENCY_CLIENT
#define NUM_OF_CORES    32
u32 * cycles_high_before;
u32 * cycles_high_after;
u32 * cycles_low_before;
u32 * cycles_low_after;

// u32 * cycles_high_before  = kzalloc(sizeof(unsigned int) * NUM_OF_CORES, GFP_KERNEL); 
// u32 * cycles_high_after   = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
// u32 * cycles_low_before   = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
// u32 * cycles_low_after    = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
#endif

/**
 * 1) Define device i/o operation
 * 
 */

/**
 * Initialize  each dispatch queue.
 * Assign the self-defined driver data to these dispatch queue.
 * 
 * 
 * Parameters
 *    hctx :  
 *        The hardware dispatch queue context.
 * 
 *    data : who assigns this parameter ??
 *        seems the data is the rdd_device_control, the driver controller/context.
 * 
 *    hw_index : 
 *        The index for the dispatch queue.
 *        The mapping between : Dispatch queue[i] --> rmem_rdma_queue[i].
 * 
 * 
 * More Explanation
 *      [?] The problem is that every hardware core will invoke blk_mq_ops->.init_hctx  ?
 *        => Sure, check ?
 *      
 *      2) Should initialize rdma_session_context->rmem_rdma_queue first.
 * 
 */
static int rmem_init_hctx(struct blk_mq_hw_ctx *hctx, void *data, unsigned int hw_index){

  int ret  = 0;

  struct rmem_device_control  *rmem_dev_ctrl = data;
  struct rmem_rdma_queue      *rdma_q_ptr = &(rmem_dev_ctrl->rdma_session->rmem_rdma_queue_list[hw_index]);  // Initialize rdma_queue first.


  #ifdef  DEBUG_RDMA_CLIENT
  if(rmem_dev_ctrl->rdma_session->rmem_rdma_queue_list == NULL){
    printk(KERN_ERR "%s, rmem_dev_ctrl->rdma_session->rmem_rdma_queue_list is NULL. Should intialize it before go to here.", __func__);
    goto err;
  }

  #endif

  // do some initialization for the  rmem_rdma_queues
  // 
  rdma_q_ptr->q_index   = hw_index;
  //rdma_q_ptr->length  = rmem_dev_ctrl->queue_depth; // Delayed intialization of rdma queue.
  #ifdef DEBUG_RDMA_CLIENT
  // Is thre any kernel defined check ?
  if(rdma_q_ptr->rdma_queue_depth != rmem_dev_ctrl->queue_depth){
    printk("%s, rdma_q_ptr->rdma_queue_depth != rmem_dev_ctrl->queue_depth \n", __func__);
    goto err;
  }
  #endif

  // Assign rdma_queue to dispatch queue as driver data.
  // Decide the mapping between dispatch queue and RDMA queues 
  hctx->driver_data = rdma_q_ptr;  // Assign the RDMA queue as dispatch queue driver data.

  return ret;

  err:
  printk(KERN_ERR "ERROR in %s. \n", __func__);
  return ret;
}





/**
 * Got a poped I/O request from dispatch queue, transfer it to 1-sided RDMA message (read/write).
 * 
 * Parameters 
 *    blk_mq_ops->queue_rq is the regsitered function to handle this request.  
 *    blk_mq_queeu_data : I/O request list in this dispatch queue.
 *    blk_mq_hw_ctx   : A dispatch queue context, control one dispatch queue.
 * 
 * More Explanation
 * 
 * [?] For the usage of bio : Documentation/block/biodoc.txt
 * 
 */
static int rmem_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd){

  int itnernal_ret = 0;
  int cpu;
  // Get the corresponding RDMA queue of this dispatch queue.
  // blk_mq_hw_ctx->driver_data  stores the RDMA connection context.
  struct rmem_rdma_queue* rdma_q_ptr = hctx->driver_data;   
  struct request *rq = bd->rq;
  

  //
  // TO BE DONE
  //
  #ifdef DEBUG_RDMA_CLIENT
  u64 seg_num   = rq->nr_phys_segments;  // number of bio ??
  u64 byte_len  = blk_rq_bytes(rq);
  struct bio      *bio_ptr;
  struct bio_vec  *bv;
  int i;

  if(rq_data_dir(rq) == WRITE){
    printk("%s: dispatch_queue[%d], get a write request, tag :%d >>>>>  \n", __func__, hctx->queue_num, rq->tag);
  }else{
    printk("%s: dispatch_queue[%d], get a read  request, tag :%d >>>>>  \n", __func__, hctx->queue_num, rq->tag);
  }
  printk("%s: <%llu> segmetns, <%u> segments in fist bio , byte length : 0x%llx \n ",__func__, seg_num,rq->bio->bi_phys_segments, byte_len);

  // printk("  :-->Forward write ?:[%d] request, tag: %d,  <%u> segments， <%u> segments of first bio ,\n",
  //                                                                                       write_or_not, rq->tag, 
  //                                                                                       rq->nr_phys_segments, 
  //                                                                                       rq->bio->bi_phys_segments);

  //
  // Print the physical information of the bio
  //
  // if(!write_or_not){
  //     
  //     for(i=0 , j=0 ;i< bio_ptr->bi_io_vec->bv_len; i+= PAGE_SIZE, j++){
  //       printk("%s, handle struct page:0x%llx \n", __func__, &bio_ptr->bi_io_vec[j] );


  //     }//for
  // }// only print read for now
  bio_ptr = rq->bio;
  bio_for_each_segment_all(bv, bio_ptr, i) {
    struct page *page = bv->bv_page;
      printk("%s: handle struct page:0x%llx >> \n", __func__, (u64)page );
  }

  #endif



  #ifndef DEBUG_BD_RDMA_SEPARATELY 

  cpu = get_cpu();
  

  // start count  the time
  #ifdef DEBUG_LATENCY_CLIENT

  // Read the rdtsc timestamp and put its value into two 32 bits variables.
  asm volatile("xorl %%eax, %%eax\n\t"
              "CPUID\n\t"
              "RDTSC\n\t"
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t"
              : "=r"(cycles_high_before[rdma_q_ptr->q_index]), "=r"(cycles_low_before[rdma_q_ptr->q_index])::"%rax", "%rbx", "%rcx",
                "%rdx");


  #endif

  #ifdef DEBUG_RDMA_CLIENT
  printk("%s: get cpu %d \n",__func__, cpu);
  #endif



  // Transfer I/O request to 1-sided RDMA messages.
  itnernal_ret = transfer_requet_to_rdma_message(rdma_q_ptr, rq);
  if(unlikely(itnernal_ret)){
      printk(KERN_ERR "%s, transfer_requet_to_rdma_message is failed. \n", __func__);
      goto err;
  }

  put_cpu();
 
  #ifdef DEBUG_RDMA_CLIENT
  printk("%s: put cpu %d \n",__func__, cpu);
  #endif
 
  #endif

  // Start the reqeust 
  // [x] Inform some hardware, we are going to handle this request
  // After the request handling is done, 
  // we need to invoke blk_mq_complete_request(request,request->errors) to notify the upper layer.

  //  TOO LATE ??
  // Sometimes, the request is already returned before we reset its request->atomic_flags 
  //blk_mq_start_request(rq);





  // 
  // [?] Handle the bio 
  //      read/write data into  the disk array.
  //
  //
  // [?] difference between request->request_queue and rmem_dev_ctl_global->request_queue ? Should be the same one ?
  //

  #ifdef DEBUG_BD_RDMA_SEPARATELY
  // Below is only for debug.
  // Finish of the i/o request 
  // [x] Let's just return a NULL data back .
  // Return or NOT is managed by the driver.
  // The content is correct OR not is checked by the applcations.
  //  
  //
  blk_mq_complete_request(rq,rq->errors);  // use 0 or rq->errors
  #endif


  return BLK_MQ_RQ_QUEUE_OK;

err:
  printk(KERN_ERR "ERROR in %s \n", __func__);
  return -1;

}



/**
 * [?] When driver finishes the file page reading, notify some where?
 * 
 */
int rmem_end_io(struct bio * bio, int err){

  // Who assigns the request as bio->bi_private ??
 // struct request * req = (struct request*)ptr_from_uint64(bio->bi_private);

  // notify i/o scheduler?
 // blk_mq_end_request(req, err);

  return err;
}



/**
 * the devicer operations
 * 
 * queue_rq : handle the queued i/o operation
 * map_queues : map the hardware dispatch queue to cpu
 * init_hctx : initialize the hardware dispatch queue ?? 
 * 
 */
static struct blk_mq_ops rmem_mq_ops = {
    .queue_rq       = rmem_queue_rq,
    .map_queues     = blk_mq_map_queues,      // Map staging queues to  hardware dispatch queues via the cpu id.
    .init_hctx      = rmem_init_hctx,
 // .complete       =                       // [?] Do we need to initialize this ?
};



/**
 * Transfer the I/O requset to 1-sided RDMA message.
 * 
 * Parameters
 *    rdma_q_ptr: the corresponding RDMA queue of this dispatch queue.
 *    rq        : the popped requset from dispatch queue.
 * 
 * 
 * More Explanation
 *  [?] In our design, we want the start_addr of file page is equal to the address of virtual pages. 
 *      How to implement this ?
 *        When a swapwrite happens, we get a file page having the same address with the virtual page.
 *         => This needs to modify the Kernel swap mechanism. 
 * 
 */
int transfer_requet_to_rdma_message(struct rmem_rdma_queue* rdma_q_ptr, struct request * rq){

  int ret = 0;
  int write_or_not          = (rq_data_dir(rq) == WRITE);
  uint64_t  start_addr      = blk_rq_pos(rq) << RMEM_LOGICAL_SECT_SHIFT;    // The start address of file page. sector_t is u64.
  uint64_t  bytes_len       = blk_rq_bytes(rq);                             //[??] The request can NOT be contiguous !!! 
  u64       debug_byte_len  = bytes_len;
  //u64       bytes_len     = PAGE_SIZE;  // For debug, read fixed 1 page.
  struct rdma_session_context  *rmda_session = rdma_q_ptr->rdma_session;

  
  struct remote_mapping_chunk   *remote_chunk_ptr;
  uint32_t  start_chunk_index   = start_addr >> CHUNK_SHIFT;   // 1GB/chunk in default.
  //uint32_t  end_chunk_index     = (start_addr + bytes_len - PAGE_SIZE) >> CHUNK_SHIFT;  // Assume start_chunk_index == end_chunk_index.
  uint32_t  end_chunk_index     = (start_addr + bytes_len - 1) >> CHUNK_SHIFT;
  //debug
  //uint32_t  end_chunk_index = start_chunk_index;

  uint64_t  offset_within_chunk     =  start_addr & CHUNK_MASK; // get the file address offset within chunk.


  #ifdef DEBUG_RDMA_CLINET 
  printk("%s: blk_rq_pos(rq):0x%llx, start_adrr:0x%llx, RMEM_LOGICAL_SECT_SHIFT: 0x%llx, CHUNK_SHIFT : 0x%llx, CHUNK_MASK: 0x%llx \n",
                                                      __func__,(u64)blk_rq_pos(rq),start_addr, (u64)RMEM_LOGICAL_SECT_SHIFT, 
                                                      (u64)CHUNK_SHIFT, (u64)CHUNK_MASK);



  
  // Assume all the read/write hits in the same chunk.
  if(start_chunk_index!= end_chunk_index){
    ret =-1;
    printk(KERN_ERR "%s, request start_add:0x%llx, byte_len:0x%llx \n",__func__, start_addr,bytes_len);
    printk(KERN_ERR "%s, start_chunk_index[%u] != end_chunk_index[%u] \n", __func__,start_chunk_index, end_chunk_index);
    
    bytes_len = rq->nr_phys_segments * PAGE_SIZE;
    printk(KERN_ERR "%s, reset byte_len(0x%llx)  to rq->nr_phys_segments * PAGE_SIZE:0x%llx \n",__func__, debug_byte_len, bytes_len);
    end_chunk_index     = (start_addr + bytes_len) >> CHUNK_SHIFT;

    if(start_chunk_index!= end_chunk_index){
      printk(KERN_ERR "%s :After reset byte_len,  start_chunk_index!= end_chunk_index, error.\n",__func__);
      goto err;
    }

    //goto err;
  }
  #endif


  //debug
  // Change all  the memory access to a specific chunk.
  // if(start_chunk_index == 7)
  //   start_chunk_index =1;
  //start_chunk_index   = 0;
  //offset_within_chunk = 0x1000;
  //end of debug


  //Get the remote_chunk information
  remote_chunk_ptr  = &(rmda_session->remote_chunk_list.remote_chunk[start_chunk_index]);

  #ifdef DEBUG_RDMA_CLIENT
  
  // printk("%s: TRANSFER TO RDMA MSG:  I/O request start_addr : 0x%llx, byte_length 0x%llx  --> \n ",__func__, start_addr, bytes_len);

  printk("%s: TO RDMA MSG[%llu], remote chunk[%u], chunk_addr 0x%llx, rkey 0x%x offset 0x%llx, byte_len : 0x%llx  \n",  
                                                      __func__ ,rmda_ops_count,  start_chunk_index,  
                                                      remote_chunk_ptr->remote_addr, remote_chunk_ptr->remote_rkey,    
                                                      offset_within_chunk, bytes_len);
  rmda_ops_count++;
    #endif

 
  // start i/o requset before set it as start.
  blk_mq_start_request(rq);


  // Build the 1-sided RDMA read/write.
  if(write_or_not){
    // post a 1-sided RDMA write
    // Into RDMA section.
    ret = post_rdma_write(rmda_session ,rq, rdma_q_ptr, remote_chunk_ptr,  offset_within_chunk, bytes_len);
    if(unlikely(ret)){
      printk(KERN_ERR "%s, post 1-sided RDMA write failed. \n", __func__);
      goto err;
    }

  }else{
    // post a 1-sided RDMA read
    ret = post_rdma_read(rmda_session, rq, rdma_q_ptr, remote_chunk_ptr,  offset_within_chunk, bytes_len);
    if(unlikely(ret)){
      printk(KERN_ERR "%s, post 1-sided RDMA read failed. \n", __func__);
      goto err;
    }

  }




  return ret;

  err:
  printk(KERN_ERR "ERROR in %s \n", __func__);
  return ret;
}









/**
 * #################3
 * 3) Register Local Block Device (Interface)
 * 
 */


/**
 * blk_mq_tag_set stores all i/o operations definition. 
 * 
 *  I/O requets control.
 *      blk_mq_tag_set->ops : Define the i/o operations.  request enqueue (staging queue), dispach queue initialization etc.
 *      blk_mq_tag_set->nr_hw_queues : number of hardware dispatch queue. usually, stage queue = dispatch queue = avaible cores 
 * 
 *      blk_mq_tag_set->driver_data   : points to driver controller.
 * 
 * [?] For the staging queue, set the information within it directly ?
 * 
 * For C:
 *  key word "static" constraint the usage scope of this function to be within this file. 
 * 
 */
static int init_blk_mq_tag_set(struct rmem_device_control* rmem_dev_ctrl){

  struct blk_mq_tag_set* tag_set = &(rmem_dev_ctrl->tag_set);
  int ret = 0;

  if(unlikely(!tag_set)){
    printk(KERN_ERR "%s, init_blk_mq_tag_set : pass a null pointer in. \n", __func__);
    ret = -1;
    goto err;
  }

  tag_set->ops = &rmem_mq_ops;
  tag_set->nr_hw_queues = rmem_dev_ctrl->nr_queues;   // hardware dispatch queue == software staging queue == avaible cores
  tag_set->queue_depth = rmem_dev_ctrl->queue_depth;  // [?] staging / dispatch queues have the same queue depth ?? or only staging queues have queue depth ?? 
  tag_set->numa_node  = NUMA_NO_NODE;
  tag_set->cmd_size = sizeof(struct rmem_rdma_command);     // [?] Send a rdma_request with the normal i/o request to remote memory server ??
  tag_set->flags = BLK_MQ_F_SHOULD_MERGE;                   // [?]merge the i/o requets ??
  tag_set->driver_data = rmem_dev_ctrl;                     // The driver controller, the context 


  ret = blk_mq_alloc_tag_set(tag_set);      // Check & correct the value within the blk_mq_tag_set.
    if (unlikely(ret)){
    pr_err("blk_mq_alloc_tag_set error. \n");
        goto err;
  }

  return ret;

err:
  pr_err(" Error in  %s \n",__func__);
  return ret;
}



/**
 * request_queue is the requst queue descriptor/cotrollor, not the real queue.
 * 
 * Real request queue:
 *    blk_mq_ctx->rq_list, the request queue in staging context. 
 *    blk_mq_hw_ctx->dispatch , is the request queue in dispatch context. 
 * 
 * Explanation
 *    blk_mq_ctx is only used by kernel. 
 *    As disk driver, we only need to set the dispatch queue.
 * 
 *    For fast block device, use bio layer directly may get better performance. 
 *    Because for fast divice, it can respond random access very fast.
 *    Enqueue/pop , merge bio/requests etc waste lots of cpu cycles. 
 */
static int init_blk_mq_queue(struct rmem_device_control* rmem_dev_ctrl){

  int ret = 0;
  int page_size;

  rmem_dev_ctrl->queue = blk_mq_init_queue(&rmem_dev_ctrl->tag_set);   // Build the block i/o queue.
    if (unlikely(IS_ERR(rmem_dev_ctrl->queue ))) {
        ret = PTR_ERR(rmem_dev_ctrl->queue );
    printk(KERN_ERR "%s, create the software staging reqeust queue failed. \n", __func__);
        goto err;
    }

  // request_queue->queuedata is reservered for driver usage.
  // works like : 
  // blk_mq_tag_set->driver_data 
  // gendisk->private_data
  rmem_dev_ctrl->queue->queuedata = rmem_dev_ctrl;
  
  //
  // * set some device queue information
  // i.e. Cat get these information from : /sys/block/sda/queue/*
  // logical block size   : 4KB, The granularity of kernel read/write. From disk buffer to memory ?
  // physical block size  : 512B aligned. The granularity read from hardware disk.  
  // 
  
  
  page_size = PAGE_SIZE;           // alignment to RMEM_SECT_SIZE

  blk_queue_logical_block_size(rmem_dev_ctrl->queue, RMEM_LOGICAL_SECT_SIZE); // logical block size, 4KB. Access granularity generated by Kernel
    blk_queue_physical_block_size(rmem_dev_ctrl->queue, RMEM_PHY_SECT_SIZE);    // physical block size, 512B. Access granularity generated by Drvier (to handware disk)
    sector_div(page_size, RMEM_LOGICAL_SECT_SIZE);                              // page_size /=RMEM_SECT_SIZE
    blk_queue_max_hw_sectors(rmem_dev_ctrl->queue, RMEM_QUEUE_MAX_SECT_SIZE);   // [?] 256kb for current /dev/sda


  return ret;

err:
  printk(KERN_ERR "Error in %s\n",__func__);
  return ret;
}

/**
 * Allocate && Set the gendisk information 
 * 
 * 
 * // manual url : https://lwn.net/Articles/25711/
 * 
 * 
 * gendisk->fops : open/close a device ? 
 *        The difference with read/write i/o operation (blk_mq_ops)??
 * 
 * 
 */


static int rmem_dev_open(struct block_device *bd, fmode_t mode)
{

  // What should this function do ?
  // open some hardware disk by path ??

    pr_debug("%s called\n", __func__);
    return 0;
}

static void rmem_dev_release(struct gendisk *gd, fmode_t mode)
{
    pr_debug("%s called\n", __func__);
}

static int rmem_dev_media_changed(struct gendisk *gd)
{
    pr_debug("%s called\n", __func__);
    return 0;
}

static int rmem_dev_revalidate(struct gendisk *gd)
{
    pr_debug("%s called\n", __func__);
    return 0;
}

static int rmem_dev_ioctl(struct block_device *bd, fmode_t mode,
              unsigned cmd, unsigned long arg)
{
    pr_debug("%s called\n", __func__);
    return -ENOTTY;
}

// [?] As a memory pool, how to assign the geometry information ?
// Assign some value like the nvme driver ?
int rmem_getgeo(struct block_device * block_device, struct hd_geometry * geo){
      pr_debug("%s called\n", __func__);
    return -ENOTTY;
}

/**
 * Device operations (for disk tools, user space behavior)
 *    open : open  device for formating ?
 *    release : delete the blockc device.
 * 
 *    getgeo  : get disk geometry. i.e. fdisk need this information.
 * 
 * more :
 *    read/write is for i/o request handle.
 * 
 */
static struct block_device_operations rmem_device_ops = {
  .owner            = THIS_MODULE,
  .open             = rmem_dev_open,
  .release          = rmem_dev_release,
  .media_changed    = rmem_dev_media_changed,
  .revalidate_disk  = rmem_dev_revalidate,
  .ioctl            = rmem_dev_ioctl,
  .getgeo           = rmem_getgeo,
};

/**
 * Allocate & intialize the gendisk.
 * The main controller for Block Device hardare.
 * 
 * [?] Show a device under /dev/
 * alloc_disk_node()  : allocate gendisk handler
 * add_disk()         : add to /dev/
 * 
 * Fields :
 *    gendisk->ops    : define device operation 
 *    gendisk->queue  : points to (staging queue) or (dispatch queue) ? Should be dispatch queue ?
 *    gendisk->private_data : driver controller.
 * 
 * [?] Do we need to set a block_device ??
 * 
 */
int init_gendisk(struct rmem_device_control* rmem_dev_ctrl ){

  int ret = 0;
  sector_t remote_mem_sector_num = RMEM_SIZE_IN_PHY_SECT; // number of physical sector

  rmem_dev_ctrl->disk = alloc_disk_node(1, NUMA_NO_NODE); // minors =1, at most have one partition.
  if(unlikely(!rmem_dev_ctrl->disk)){
    printk("%s: Failed to allocate disk node\n", __func__);
        ret = -ENOMEM;
    goto err;
  }

  rmem_dev_ctrl->disk->major  = rmem_dev_ctrl->major;
  rmem_dev_ctrl->disk->first_minor = 0;                 // The partition id, start from 0.
  rmem_dev_ctrl->disk->fops   = &rmem_device_ops;       // Define device operations
  rmem_dev_ctrl->disk->queue  = rmem_dev_ctrl->queue;   // Assign the hardware dispatch queue
  rmem_dev_ctrl->disk->private_data = rmem_dev_ctrl;    // Driver controller/context. Reserved for disk driver.
  memcpy(rmem_dev_ctrl->disk->disk_name, rmem_dev_ctrl->dev_name, DEVICE_NAME_LEN);

  // RMEM_SIZE_IN_PHY_SECT is just the number of physical sector already.
  //sector_div(remote_mem_sector_num, RMEM_LOGICAL_SECT_SIZE);    // remote_mem_size /=RMEM_SECT_SIZE, return remote_mem_size%RMEM_SECT_SIZE 
  set_capacity(rmem_dev_ctrl->disk, remote_mem_sector_num);     // size is in remote file state->size, add size info into block device
 
 
  // Register Block Device through gendisk(->partition)
  // Device will show under /dev/
  // After call this function, disk is active and prepared well for any i/o request.
  add_disk(rmem_dev_ctrl->disk);

  #ifdef DEBUG_RDMA_CLIENT
  printk("init_gendisk : initialize disk %s done. \n", rmem_dev_ctrl->disk->disk_name);
  #endif

  return ret;

err:
  printk(KERN_ERR "ERROR in %s \n",__func__);
  del_gendisk(rmem_dev_ctrl->disk);
  return ret;
}

/**
 *  Allocate and initialize the fields of Driver controller/context.
 * 
 *  [?] What's the necessary fields ?
 * 
 */
int init_rmem_device_control(char* dev_name, struct rmem_device_control* rmem_dev_ctrl){
  
  int ret = 0;

  // if driver controller is null, create a new driver context.
  //if(!rmem_dev_ctrl){

    //
    // [?] Can we don the allocation during kernel booting time ??
    // 
    //rmem_dev_ctrl = (struct rmem_device_control *)kzalloc(sizeof(struct rmem_device_control), GFP_KERNEL);  // kzlloc, initialize memory to zero.
    if(rmem_dev_ctrl == NULL){
      ret = -1;
      pr_err("Allocate struct rmem_device_control failed \n");
      goto out;
    }
 // }

  if(dev_name != NULL ){
    //Use assigned name
    int len = strlen(dev_name) >= DEVICE_NAME_LEN ? DEVICE_NAME_LEN : strlen(dev_name);
    memcpy(rmem_dev_ctrl->dev_name, dev_name, len);   // string copy or memory copy ?
  }else{
    strcpy(rmem_dev_ctrl->dev_name,"rmempool");       // rmem_dev_ctrl->dev_name = "xx" is wrong. Try to change the value(address) of a char pointer.
  }

  rmem_dev_ctrl->major = rmem_major_num;

  rmem_dev_ctrl->queue_depth  = RMEM_QUEUE_DEPTH; // 
  rmem_dev_ctrl->nr_queues    = online_cores;     // staging queue = dispatch queue = avaible cores.

  rmem_dev_ctrl->freed       = 0;                // Falg, if the block resouce is already freed.
  //
  // blk_mq_tag_set, request_queue, rmem_rdma_queue are waiting to be initialized later. 
  //

out:
  return ret;
}




/**
 * The main function to create the block device. 
 * 
 * Parameters:
 *    device name :
 *    driver context : self-defiend structure. 
 *          [?] who assigns the value of rmem_dev_ctrl ??
 * 
 * Explanation
 *    1) Build and initialize the driver context.
 *          [?] What fileds need to be defined and initialized ?
 *    2) Define i/o operation
 *    3) Allocate and initialize the i/o staging & dispatch queues
 *    4) Active the block device : add it tho kernel bd list.
 * 
 * return 0 if success.
 */
int RMEM_create_device(char* dev_name, struct rmem_device_control* rmem_dev_ctrl ){

  int ret = 0;  
  //unsigned long page_size; 

  //
  //  1) Initiaze the fields of rmem_device_control structure 
  //
  if(rmem_dev_ctrl == NULL ){    
    pr_err("Can not pass a null pointer to initialize. \n");
    ret = -1;
    goto out;
  }

  ret = init_rmem_device_control( dev_name, rmem_dev_ctrl);
  if(ret){
    pr_err("Intialize rmem_device_control error \n");
    goto out;
  }

  // 2) Intialize thte blk_mq_tag_set
  ret = init_blk_mq_tag_set(rmem_dev_ctrl);
  if(ret){
    printk("Allocate blk_mq_tag_set failed. \n");
   goto error;
  }


  //
  // 3) allocate & intialize the software staging queue
  //
  ret = init_blk_mq_queue(rmem_dev_ctrl);
  if(ret){
    printk("init_blk_mq_queue failed. \n");
    goto error;
  }
  
  //
  // 4) initiate the gendisk (device information) & add the device into kernel list (/dev/) -- Active device..
  //
  ret = init_gendisk(rmem_dev_ctrl);
  if(ret){
    pr_err("Init_gendisk failed \n");
    goto error;
  }



  //debug
  printk("RMEM_create_device done. \n");

out:
  return ret;

error:
  pr_err("Error in RMEM_create_device \n");
  return ret;
}




/**
 * The entry of disk driver.
 * Invoked by RDMA part after the RDMA connection is built.
 * 
 * Can NOT be static, this funtion is invoked in rdma_client.c.
 * 
 * More Explanation
 *    1) online_cores is initialized in rdma part.
 */
 int  rmem_init_disk_driver(struct rmem_device_control *rmem_dev_ctl){

  int ret = 0;
    
  #ifdef  DEBUG_RDMA_CLIENT 
  printk("%s,Load kernel module : register remote block device. \n", __func__);
  #endif

  #ifdef DEBUG_LATENCY_CLIENT
  cycles_high_before  = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL); 
  cycles_high_after   = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
  cycles_low_before   = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
  cycles_low_after    = kzalloc(sizeof(u32) * NUM_OF_CORES, GFP_KERNEL);
  #endif

  // Become useless since 4.9, maybe removed latter.
  // Get a major number, list the divice under /proc/devices
  // i.e.
  // <major_number> <device_name>
  // 252 rmempool
  //
    rmem_major_num = register_blkdev(0, "rmempool");
    if (unlikely(rmem_major_num < 0)){
    printk(KERN_ERR "%s, register_blkdev failed. \n",__func__);
    ret = -1;
        goto err;
  }

  // number of staging and dispatch queues, equal to available cores
  // Kernel blocking layer assume that the number of software staging queue and avialable cores are same. 
  // Usualy, we set the number of dispatch queues same with staging queues.
  //online_cores = num_online_cpus(); // Initialized in rdma part.
  
  #ifdef  DEBUG_RDMA_CLIENT 
  printk("%s, Got block device major number: %d \n", __func__, rmem_major_num);
  printk("%s, online cores : %d \n", __func__, online_cores);
  #endif

  //debug
  // create the block device here
  // Create block information within functions
  
  ret = RMEM_create_device(NULL, rmem_dev_ctl);  
  if(unlikely(ret)){
    printk(KERN_ERR "%s, Crate block device error.\n", __func__);
    goto err;
  }

  return ret;

err:
  printk(KERN_ERR "ERROR in %s \n", __func__);
    return ret;
}




/**
 * 
 * Blcok Resource free
 * 
 */

// This function can only be called once.
//  And the rmem_dev_ctrl context is initialized.
int octopus_free_block_devicce(struct rmem_device_control * rmem_dev_ctrl ){
  int ret =0;

  if( rmem_dev_ctrl != NULL && rmem_dev_ctrl->freed != 0){
    //already called 
    return ret;
  }

  rmem_dev_ctrl->freed  = 1;

  if(rmem_dev_ctrl!=NULL){


    if(likely(rmem_dev_ctrl->disk != NULL)){
      del_gendisk(rmem_dev_ctrl->disk);
      printk("%s, free gendisk done. \n",__func__);
    }
    
    if(likely(rmem_dev_ctrl->queue != NULL)){
      blk_cleanup_queue(rmem_dev_ctrl->queue);
      printk("%s, free and close dispatch queue. \n",__func__);
    }

    blk_mq_free_tag_set(&(rmem_dev_ctrl->tag_set));
    printk("%s, free tag_set done. \n",__func__);

    if(likely(rmem_dev_ctrl->disk != NULL)){
        put_disk(rmem_dev_ctrl->disk);
      printk("%s, put_disk gendisk done. \n",__func__);
    }

    unregister_blkdev(rmem_dev_ctrl->major, rmem_dev_ctrl->dev_name);

    printk("%s, Free Block Device, %s, DONE. \n",__func__, rmem_dev_ctrl->dev_name);

  }else{
      printk("%s, rmem_dev_ctrl is NULL, nothing to free \n",__func__);
  }

  return ret;
}



// module function
// static void RMEM_cleanup_module(void){

//     unregister_blkdev(rmem_major_num, "rmempool");

// }




//
// RDMA part invoke the device registration funtion.
//

//module_init(RMEM_init_module);
//module_exit(RMEM_cleanup_module);
