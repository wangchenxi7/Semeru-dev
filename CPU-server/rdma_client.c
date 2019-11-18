/**
 * rdma_client.c is used for translating the I/O requests to RDMA messages 
 * 	and sent them to remote memory server via InfiniBand.
 * 
 * The kernel module initializes  global variables and register stateless functions to the RDMA driver.
 * 
 * 1) Global variables
 * 		a. struct rdma_session_context		rdma_seesion_global; 
 * 			contains all the RDMA controllers, rdma_cm_id, ib_qp, ib_cq, ib_pd etc.
 * 			These structures are used to maintain the RDMA connection and transportation.
 * 			This is a global variable which exist in static area. 
 * 			Even when the main function exits, this variable can exist can works well.
 * 		b. struct rmem_device_control  	rmem_dev_ctrl_global; 
 * 			Used for Block Device controlling.
 * 
 * 2) Handler fucntions
 * 		octopus_rdma_cm_event_handler, octopus_cq_event_handler are the 2 main stateless handlers.
 * 			a. octopus_rdma_cm_event_handler is registered to the RDMA driver(mlx4) to handle all the RDMA communication evetns.
 * 			b. octopus_cq_event_handler is registered to RDMA Completion Queue(CQ) to handle the received/sent RDMA messages.
 * 	
 * 		These 2 functions are stateless function. Even when the main function of the kernel module exits, these two function works well. 
 * 		We can use the notify-mode to avoid maintaining a polling daemon function.
 * 
 * 3) Daemon threads
 * 		Under the desing of Notify-CQ mode, don't see the need of deamon thread design.
 * 
 * 
 */

#include "semeru_cpu.h"


MODULE_AUTHOR("Excavator,plsys");
MODULE_DESCRIPTION("RMEM, remote memory paging over RDMA");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION("1.0");

//
// Implement the global vatiables here
//

struct rdma_session_context 	rdma_session_global;
int online_cores;

//debug
u64	rmda_ops_count	= 0;
u64	cq_notify_count	= 0;
u64	cq_get_count	= 0;
//
// ############# Start of RDMA Communication (CM) event handler ########################
//
// [?] All the RDMA Communication(CM) event is triggered by hardware and mellanox driver.
//		No need to maintain a daemon thread to handle the CM events ?  -> stateless handler 
//


/**
 * The rdma CM event handler function
 * 
 * [?] Seems that this function is triggered when a CM even arrives this device. 
 * 
 * More Explanation
 * 	 CMA Event handler  && cq_event_handler , 2 different functions for CM event and normal RDMA message handling.
 * 
 */
int octopus_rdma_cm_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event)
{
	int ret;
	struct rdma_session_context *rdma_session = cma_id->context;


	#ifdef DEBUG_RDMA_CLIENT
	pr_info("cma_event type %d, type_name: %s \n", event->event, rdma_cm_message_print(event->event));
	#endif

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		rdma_session->state = ADDR_RESOLVED;

		#ifdef DEBUG_RDMA_CLIENT
		printk("%s,  get RDMA_CM_EVENT_ADDR_RESOLVED. Send RDMA_ROUTE_RESOLVE to Memory server \n",__func__);
		#endif 

		// Go to next step, resolve the rdma route.
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR "%s,rdma_resolve_route error %d\n", __func__, ret);
		//	wake_up_interruptible(&rdma_session->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:

		#ifdef DEBUG_RDMA_CLIENT
		// RDMA route is solved, wake up the main process  to continue.
    	printk("%s : RDMA_CM_EVENT_ROUTE_RESOLVED, wake up rdma_session->sem\n ",__func__);
		#endif	

		// Sequencial controll 
		rdma_session->state = ROUTE_RESOLVED;
		wake_up_interruptible(&rdma_session->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:		// Receive RDMA connection request
		//rdma_session->state = CONNECT_REQUEST;

    	printk("Receive but Not Handle : RDMA_CM_EVENT_CONNECT_REQUEST \n");
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
	    printk("%s, ESTABLISHED, wake up kernel_cb->sem\n", __func__);

		rdma_session->state = CONNECTED;
    wake_up_interruptible(&rdma_session->sem);		

		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR "%s, cma event %d, event name %s, error code %d \n", __func__, event->event,
														rdma_cm_message_print(event->event), event->status);
		rdma_session->state = ERROR;
		wake_up_interruptible(&rdma_session->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:	//should get error msg from here
		printk( "%s, Receive DISCONNECTED  signal \n",__func__);
		//rdma_session->state = CM_DISCONNECT;

		if(rdma_session->freed){ // 1, during free process.
			// Client request for RDMA disconnection.
			#ifdef DEBUG_RDMA_CLIENT
			printk("%s, RDMA disconnect evetn, requested by client. \n",__func__);
			#endif

			// Wakeup the caller.
			// wake_up_interruptible(&rdma_session->sem);



		}else{					// freed ==0, newly start free process
			// Remote server requests for disconnection.

			// !! DEAD PATH NOW --> CAN NOT FREE CM_ID !!
			// wait for RDMA_CM_EVENT_TIMEWAIT_EXIT ??
			//			TO BE DONE

			#ifdef DEBUG_RDMA_CLIENT
			printk("%s, RDMA disconnect evetn, requested by client. \n",__func__);
			#endif
			//do we need to inform the client, the connect is broken ?
			rdma_disconnect(rdma_session->cm_id);

			octopus_disconenct_and_collect_resource(rdma_session);  	// Free RDMA resource and exit main function
			octopus_free_block_devicce(rdma_session->rmem_dev_ctrl);	// Free block device resource 
		}
		break;

	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		// After the received DISCONNECTED_EVENT, need to wait the on-the-fly RDMA message
		// https://linux.die.net/man/3/rdma_get_cm_event

		printk("%s, Wait for in-the-fly RDMA message finished. \n",__func__);
		rdma_session->state = CM_DISCONNECT;

		//Wakeup caller
		wake_up_interruptible(&rdma_session->sem);

		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:	//this also should be treated as disconnection, and continue disk swap
		printk(KERN_ERR "%s, cma detected device removal!!!!\n", __func__);
		return -1;
		break;

	default:
		printk(KERN_ERR "%s,oof bad type!\n",__func__);
		wake_up_interruptible(&rdma_session->sem);
		break;
	}

	return ret;
}




// Resolve the destination IB device by the destination IP.
// [?] Need to build some route table ?
// 
static int rdma_resolve_ip_to_ib_device(struct rdma_session_context *rdma_session)
{
	struct sockaddr_storage sin; 
	int ret;

	//fill_sockaddr(&sin, cb);
	// Assume that it's ipv6
	// [?]cast "struct sockaddr_storage" to "sockaddr_in" ??
	//
	struct sockaddr_in *sin4 = (struct sockaddr_in *)&sin;   
	sin4->sin_family = AF_INET;
	memcpy((void *)&(sin4->sin_addr.s_addr), rdma_session->addr, 4);   	// copy 32bits/ 4bytes from cb->addr to sin4->sin_addr.s_addr
	sin4->sin_port = rdma_session->port;                             		// assign cb->port to sin4->sin_port


	ret = rdma_resolve_addr(rdma_session->cm_id, NULL, (struct sockaddr *)&sin, 2000); // timeout 2000ms
	if (ret) {
		printk(KERN_ERR "%s, rdma_resolve_ip_to_ib_device error %d\n", __func__, ret);
		return ret;
	}else{
		printk("rdma_resolve_ip_to_ib_device - rdma_resolve_addr success.\n");
	}
	
	// Wait for the CM events to be finished:  handled by rdma_cm_event_handler()
	// 	1) resolve addr
	//	2) resolve route
	// Come back here and continue:
	//
	wait_event_interruptible(rdma_session->sem, rdma_session->state >= ROUTE_RESOLVED);   //[?] Wait on cb->sem ?? Which process will wake up it.
	if (rdma_session->state != ROUTE_RESOLVED) {
		printk(KERN_ERR  "%s, addr/route resolution did not resolve: state %d\n", __func__, rdma_session->state);
		return -EINTR;
	}
	printk("rdma_resolve_ip_to_ib_device -  resolve address and route successfully\n");
	return ret;
}



/**
 * Build the Queue Pair (QP).
 * 
 */
int octopus_create_qp(struct rdma_session_context *rdma_session)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = rdma_session->txdepth; /*FIXME: You may need to tune the maximum work request */
	init_attr.cap.max_recv_wr = rdma_session->txdepth;  
	init_attr.cap.max_recv_sge = MAX_REQUEST_SGL;					// enable the scatter
	init_attr.cap.max_send_sge = MAX_REQUEST_SGL;					// enable the gather 
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;     // Receive WR ?
	init_attr.qp_type = IB_QPT_RC;                // Queue Pair connect type, Reliable Communication.  [?] Already assign this during create cm_id.

	// [?] Can both recv_cq and send_cq use the same cq ??
	init_attr.send_cq = rdma_session->cq;
	init_attr.recv_cq = rdma_session->cq;

	ret = rdma_create_qp(rdma_session->cm_id, rdma_session->pd, &init_attr);
	if (!ret){
		// Record this queue pair.
		rdma_session->qp = rdma_session->cm_id->qp;
  	}else{
    	printk(KERN_ERR "%s:  Create QP falied. errno : %d \n", __func__, ret);
  	}

	return ret;
}





// Prepare for building the Connection to remote IB servers. 
// Create Queue Pair : pd, cq, qp, 
int octopus_create_rdma_queues(struct rdma_session_context *rdma_session, struct rdma_cm_id *cm_id)
{
	int ret = 0;

	struct ib_cq_init_attr init_attr;
	// 1) Build PD.
	// flags of Protection Domain, (ib_pd) : Protect the local OR remote memory region ? ?  
	// Local Read is default.
	rdma_session->pd = ib_alloc_pd(cm_id->device, IB_ACCESS_LOCAL_WRITE|
                                            		IB_ACCESS_REMOTE_READ|
                                            		IB_ACCESS_REMOTE_WRITE );    // No local read ??  [?] What's the cb->pd used for ?

	if (IS_ERR(rdma_session->pd)) {
		printk(KERN_ERR "%s, ib_alloc_pd failed\n", __func__);
		return PTR_ERR(rdma_session->pd);
	}
	pr_info("%s, created pd %p\n", __func__, rdma_session->pd);

	// 2) Build CQ
	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cqe = rdma_session->txdepth * 2;     // [?] The depth of cq. Number of completion queue entries.  ??
	init_attr.comp_vector = 0;					   // [?] What's the meaning of this ??
	
	// Set up the completion queues and the cq evnet handler.
	// Parameters
	// 		cq_context = qp_context = rdma_session_context.
	//
	// [?] receive cq and send cq are the same one ??
	// 		ib_qp->send_cq, ib_qp->recv_cq seperately. 
	//		How about we create these 2 queues ?  = Seems the nvme_over_fabrics also use the single cq.
	// 
	rdma_session->cq = ib_create_cq(cm_id->device, octopus_cq_event_handler, NULL, rdma_session, &init_attr);

	if (IS_ERR(rdma_session->cq)) {
		printk(KERN_ERR "%s, ib_create_cq failed\n", __func__);
		ret = PTR_ERR(rdma_session->cq);
		goto err;
	}
	pr_info("%s, created cq %p\n", __func__, rdma_session->cq);

  	// Request a notification (IRQ), if an event arrives on CQ entry.
	// ret = ib_req_notify_cq(rdma_session->cq, IB_CQ_NEXT_COMP);   
	// if (ret) {
	// 	printk(KERN_ERR "%s, ib_create_cq failed\n", __func__);
	// 	goto err;
	// }

	// 3) Build QP.
	ret = octopus_create_qp(rdma_session);
	if (ret) {
		printk(KERN_ERR  "%s, failed: %d\n", __func__, ret);
		goto err;
	}
	pr_info("%s, created qp %p\n", __func__, rdma_session->qp);
	return ret;

err:
	//ib_dealloc_pd(cb->pd);
  printk(KERN_ERR "Error in %s \n", __func__);
	return ret;
}





/**
 * Reserve two RDMA wr for receive/send meesages
 * 		rdma_session_context->rq_wr
 * 		rdma_session_context->send_sgl
 * Post these 2 WRs to receive/send controll messages.
 */
void octopus_setup_message_wr(struct rdma_session_context *rdma_context)
{
	// 1) Reserve a wr to receive RDMA message
	rdma_context->recv_sgl.addr = rdma_context->recv_dma_addr;      
	rdma_context->recv_sgl.length = sizeof(struct message);
	if (rdma_context->qp->device->local_dma_lkey){                            // check ?
		rdma_context->recv_sgl.lkey = rdma_context->qp->device->local_dma_lkey;

		#ifdef DEBUG_RDMA_CLIENT
		printk("%s, get lkey from rdma_context->local_dma_lkey \n",__func__);
		#endif
	}else if (rdma_context->mem == DMA){
		rdma_context->recv_sgl.lkey = rdma_context->dma_mr->lkey;  //[?] Why not use local_dma_lkey

		#ifdef DEBUG_RDMA_CLIENT
		printk("%s, get lkey from rdma_context->dma_mr->lkey \n",__func__);
		#endif
	}
	rdma_context->rq_wr.sg_list = &rdma_context->recv_sgl;
	rdma_context->rq_wr.num_sge = 1;


	// 2) Reserve a wr
	rdma_context->send_sgl.addr = rdma_context->send_dma_addr;
	rdma_context->send_sgl.length = sizeof(struct message);
	if (rdma_context->qp->device->local_dma_lkey){
		rdma_context->send_sgl.lkey = rdma_context->qp->device->local_dma_lkey;
	}else if (rdma_context->mem == DMA){
		rdma_context->send_sgl.lkey = rdma_context->dma_mr->lkey;
	}
	rdma_context->sq_wr.opcode = IB_WR_SEND;		// ib_send_wr.opcode , passed to wc.
	rdma_context->sq_wr.send_flags = IB_SEND_SIGNALED;
	rdma_context->sq_wr.sg_list = &rdma_context->send_sgl;
	rdma_context->sq_wr.num_sge = 1;

}



/**
 * We reserve two WRs for send/receive RDMA messages in a 2-sieded way.
 * 	a. Allocate 2 buffers
 * 		dma_session->recv_buf
 * 		rdma_session->send_buf
 *	b. Bind their DMA/BUS address to  
 * 		rdma_context->recv_sgl
 * 		rdma_context->send_sgl
 * 	c. Bind the ib_sge to send/receive WR
 * 		rdma_context->rq_wr
 * 		rdma_context->sq_wr
 */
int octopus_setup_buffers(struct rdma_session_context *rdma_session)
{
	int ret;

	// 1) Allocate some DMA buffers.
	// [x] Seems that any memory can be registered as DMA buffers, if they satisfy the constraints:
	// 1) Corresponding physical memory is allocated. The page table is built. 
	//		If the memory is allocated by user space allocater, malloc, we need to walk  through the page table.
	// 2) The physial memory is pinned, can't be swapt/paged out.
  rdma_session->recv_buf = kzalloc(sizeof(struct message), GFP_KERNEL);  	//[?] Or do we need to allocate DMA memory by get_dma_addr ???
	rdma_session->send_buf = kzalloc(sizeof(struct message), GFP_KERNEL);  
	rdma_session->mem = DMA;   // [??] Is this useful ? Get lkey fomr rmem_session_context->dma_mr.


	// Get DMA/BUS address for the receive buffer
	rdma_session->recv_dma_addr = ib_dma_map_single(rdma_session->pd->device, rdma_session->recv_buf, sizeof(struct message), DMA_BIDIRECTIONAL);
	//pci_unmap_addr_set(rdma_session, recv_mapping, rdma_session->recv_dma_addr);   //	Replicate MACRO DMA assign


//	cb->send_dma_addr = dma_map_single(&cb->pd->device->dev, 
//				   &cb->send_buf, sizeof(struct message), DMA_BIDIRECTIONAL);	

	rdma_session->send_dma_addr = ib_dma_map_single(rdma_session->pd->device, rdma_session->send_buf, sizeof(struct message), DMA_BIDIRECTIONAL);	
	//pci_unmap_addr_set(rdma_session, send_mapping, rdma_session->send_dma_addr);	//	Replicate MACRO DMA assign

	#ifdef DEBUG_RDMA_CLIENT
	printk("%s, Got dma/bus address 0x%llx, for the recv_buf 0x%llx \n", __func__, (unsigned long long)rdma_session->recv_dma_addr, 
																			(unsigned long long)rdma_session->recv_buf);
	printk("%s, Got dma/bus address 0x%llx, for the send_buf 0x%llx \n", __func__, (unsigned long long)rdma_session->send_dma_addr, 
																			(unsigned long long)rdma_session->send_buf);
	#endif

	
	// 2) Allocate a DMA Memory Region.
	//pr_info(PFX "rdma_session->mem=%d \n", cb->mem);

	if (rdma_session->mem == DMA) {
		// [??] What's this region used for ?
		//		=> get a lkey from rdma_session->dma_mr->lkey
		// 
		// [x] RDMA read/write . But for this, we only need a remote addr AND rkey ?
		// 

		pr_info("%s, IS_setup_buffers, in cb->mem==DMA \n", __func__);
		rdma_session->dma_mr = rdma_session->pd->device->get_dma_mr(rdma_session->pd, IB_ACCESS_LOCAL_WRITE|
							        													IB_ACCESS_REMOTE_READ|
							        													IB_ACCESS_REMOTE_WRITE);

		if (IS_ERR(rdma_session->dma_mr)) {
			pr_info("%s, reg_dmamr failed\n", __func__);
			ret = PTR_ERR(rdma_session->dma_mr);
			goto error;
		}
	} 
	

	// 3) Add the allocated (DMA) buffer to reserved WRs
	octopus_setup_message_wr(rdma_session);
	pr_info("%s, allocated & registered buffers...\n", __func__);

	return ret;


error:

	printk(KERN_ERR "%s, Bind DMA buffer error. \n", __func__);

	// if (cb->rdma_mr && !IS_ERR(cb->rdma_mr))
	// 	ib_dereg_mr(cb->rdma_mr);
	 if (rdma_session->dma_mr && !IS_ERR(rdma_session->dma_mr))
	 	ib_dereg_mr(rdma_session->dma_mr);
	// if (cb->recv_mr && !IS_ERR(cb->recv_mr))
	// 	ib_dereg_mr(cb->recv_mr);
	// if (cb->send_mr && !IS_ERR(cb->send_mr))
	// 	ib_dereg_mr(cb->send_mr);
	
	return ret;
}



/**
 * All the PD, QP, CP are setted up, connect to remote IB servers.
 * This will send a CM event to remote IB server && get a CM event response back.
 */
int octopus_connect_remote_memory_server(struct rdma_session_context *rdma_session)
{
	struct rdma_conn_param conn_param;
	int ret;

	// [?] meaning of these parameters ?
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(rdma_session->cm_id, &conn_param);  // RDMA CM event 
	if (ret) {
		printk(KERN_ERR "%s, rdma_connect error %d\n", __func__, ret);
		return ret;
	}else{
		printk("%s, Send RDMA connect request to remote server \n", __func__);
	}

	wait_event_interruptible(rdma_session->sem, rdma_session->state >= CONNECTED);
	if (rdma_session->state == ERROR) {
		printk(KERN_ERR "%s, wait for CONNECTED state %d\n", __func__, rdma_session->state);
		return ret;
	}

	pr_info("%s, RDMA connect successful\n", __func__);
	return ret;
}


//
// <<<<<<<<<<<<<<<<<<<<<<<  End of RDMA Communication (CM) event handler <<<<<<<<<<<<<<<<<<<<<<<
//






//
// >>>>>>>>>>>>>>>>>>>>>>  Start of handle  TWO-SIDED RDMA message section >>>>>>>>>>>>>>>>>>>>>>
//


/**
 * RDMA  CQ event handler.
 * After invoke the cq_notify, everytime a wc is insert into completion queue entry, 
 * notify to the process by invoking "rdma_cq_event_handler".
 * 
 * 
 * [x] For the 1-sided RDMA read/write, there is also a WC to acknowledge the finish of this.
 * 
 * 
 * 
 * 
 */
void octopus_cq_event_handler(struct ib_cq * cq, void *rdma_ctx){    // cq : kernel_cb->cq;  ctx : cq->context, just the kernel_cb

	bool 	 												stop_waiting_on_cq 	= false;
	struct rdma_session_context 	*rdma_session				=	rdma_ctx;
	struct ib_wc 									wc;
	//struct ib_recv_wr 	*bad_wr;
	int ret = 0;
	BUG_ON(rdma_session->cq != cq);
	if (rdma_session->state == ERROR) {
		printk(KERN_ERR "%s, cq completion in ERROR state\n", __func__);
		return;
	}

	#ifdef DEBUG_RDMA_CLIENT
	printk("%s: Receive cq[%llu] \n", __func__, cq_get_count++);
	#endif

	// Notify_cq, poll_cq are all both one-shot
	// Get notification for the next one or several wc.
	ret = ib_req_notify_cq(rdma_session->cq, IB_CQ_NEXT_COMP);   
	if (unlikely(ret)) {
		printk(KERN_ERR "%s: request for cq completion notification failed \n",__func__);
		goto err;
	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printk("%s: cq_notify_count : %llu , wait for next cq_event \n",__func__, cq_notify_count++);
	}
	#endif

	// If current function, rdma_cq_event_handler, is invoked, one or several WC is on the CQE.
	// Get the SIGNAL, WC, by invoking ib_poll_cq.
	//
	//drivers/infiniband/hw/mlx4/main.c:2707:	ibdev->ib_dev.poll_cq		= mlx4_ib_poll_cq;
	//	ib_poll_cq is cq->device->poll_cq

	//if(likely( (ret = ib_poll_cq(rdma_session->cq, 1, &wc)) == 1  )) {   //ib_poll_cq, get "one" wc from cq.
	while(likely( (ret = ib_poll_cq(rdma_session->cq, 1, &wc)) == 1  )) {
		if (wc.status != IB_WC_SUCCESS) {   		// IB_WC_SUCCESS == 0
			// if (wc.status == IB_WC_WR_FLUSH_ERR) {
			// 	printk(KERN_ERR "%s, cq flushed\n", __func__);
			// 	//continue;
			// 	// IB_WC_WR_FLUSH_ERR is different ??
			// 	goto err;
			// } else {
				printk(KERN_ERR "%s, cq completion failed with wr_id 0x%llx status %d,  status name %s, opcode %d,\n",__func__,
																wc.wr_id, wc.status, rdma_wc_status_name(wc.status), wc.opcode);
				
				//print the rmda_command information
				struct rmem_rdma_command *rdma_cmd_ptr = (struct rmem_rdma_command *)(wc.wr_id);
				printk(KERN_ERR "%s, ERROR i/o request->tag : %d \n", __func__,  rdma_cmd_ptr->io_rq->tag );

				goto err;
			//}
		}	

		switch (wc.opcode){
			case IB_WC_RECV:				
				#ifdef DEBUG_RDMA_CLIENT
				printk("%s, Got a WC from CQ, IB_WC_RECV. \n", __func__);
				#endif
				// Need to do actions based on the received message type.
				ret = handle_recv_wr(rdma_session, &wc);
			  	if (unlikely(ret)) {
				 	printk(KERN_ERR "%s, recv wc error: %d\n", __func__, ret);
				 	goto err;
				}

				 // debug
				 // Stop waiting for message.
				 stop_waiting_on_cq = true;

				 // Modify the state in handle_recv_wr.
				 //rdma_session->state = FREE_MEM_RECV;

				 // [?] Which function is waiting here ?
				 //wake_up_interruptible(&rdma_session->sem);

				 //ret = ib_post_recv(rdma_session->qp, &rdma_session->rq_wr, &bad_wr);
				 //if (ret) {
				 //	printk(KERN_ERR PFX "post recv error: %d\n",    ret);
				 //	goto error;
				 //}
				// if (cb->state == RDMA_BUF_ADV || cb->state == FREE_MEM_RECV || cb->state == WAIT_OPS){
				// 	wake_up_interruptible(&cb->sem);
				// }

				break;
			case IB_WC_SEND:

				// nothing need to be done for a IB_WC_SEND.

				// ret = client_send(cb, &wc);
				// if (ret) {
				// 	printk(KERN_ERR PFX "send wc error: %d\n", ret);
				// 	goto error;
				// }

				#ifdef DEBUG_RDMA_CLIENT
				printk("%s, Got a WC from CQ, IB_WC_SEND, then wait for receive RDMA mesage.. \n", __func__);
				#endif

				 break;
			case IB_WC_RDMA_READ:
				// 1-sided RDMA read is done. 
				// The data is in registered RDMA buffer.
				#ifdef DEBUG_RDMA_CLIENT
				printk("%s, Got a WC from CQ, IB_WC_RDMA_READ \n", __func__);
				#endif
				 
				 // Read data from RDMA buffer and responds it back to Kernel.
				 ret = rdma_read_done( &wc);
				 if (unlikely(ret)) {
				 	printk(KERN_ERR "%s, Handle cq event, IB_WC_RDMA_READ, error \n", __func__);
				 	goto err;
				 }
				break;
			case IB_WC_RDMA_WRITE:
				ret = rdma_write_done(&wc);
				 if (unlikely(ret)) {
				 	printk(KERN_ERR "%s, Handle cq event, IB_WC_RDMA_WRITE, error \n", __func__);
				 	goto err;
				 }

				#ifdef DEBUG_RDMA_CLIENT
				printk("%s, Got a WC from CQ, IB_WC_RDMA_WRITE \n", __func__);
				#endif

				break;
			default:
				printk(KERN_ERR "%s:%d Unexpected opcode %d, Shutting down\n", __func__, __LINE__, wc.opcode);
				goto err;
		} // switch

		//
		// Notify_cq, poll_cq are all both one-shot
		// Get notification for the next wc.
		// ret = ib_req_notify_cq(rdma_session->cq, IB_CQ_NEXT_COMP);   
		// if (unlikely(ret)) {
		// 	printk(KERN_ERR "%s: request for cq completion notification failed \n",__func__);
		// 	goto err;
		// }
		// #ifdef DEBUG_RDMA_CLIENT
		// else{
		// 	printk("%s: cq_notify_count : %llu , wait for next cq_event \n",__func__, cq_notify_count++);
		// }
		// #endif

	} // poll 1 cq   in a loop.
	// else{
	// 	printk(KERN_ERR "%s, poll error %d\n", __func__, ret);
	// 	goto err;
	// }

	return;
err:
	printk(KERN_ERR "ERROR in %s \n",__func__);
	rdma_session->state = ERROR;
	octopus_disconenct_and_collect_resource(rdma_session);  // Disconnect and free all the resource.
	return;
}




/**
 * Send a RDMA message to remote server.
 * 
 * 
 */
int send_message_to_remote(struct rdma_session_context *rdma_session, int messge_type  , int size_gb)
{
	int ret = 0;
	struct ib_send_wr * bad_wr;
	rdma_session->send_buf->type = messge_type;
	rdma_session->send_buf->size_gb = size_gb; 		// chunk_num = size_gb/chunk_size

	#ifdef DEBUG_RDMA_CLIENT
	printk("Send a Message to Remote memory server. cb->send_buf->type : %d, %s \n", messge_type, rdma_message_print(messge_type) );
	#endif

	ret = ib_post_send(rdma_session->qp, &rdma_session->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR "%s: BIND_SINGLE MSG send error %d\n", __func__, ret);
		return ret;
	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printk("%s: 2-sided RDMA message[%llu] send. \n",__func__,rmda_ops_count++);
	}
	#endif

	return ret;	
}





/**
 * Receive a WC, IB_WC_RECV.
 * Read the data from the posted WR.
 * 		For this WR, its associated DMA buffer is rdma_session_context->recv_buf.
 * 
 * Action 
 * 		According to the RDMA message information, rdma_session_context->state, to set some fields.
 * 		FREE_SIZE : set the 
 */
int handle_recv_wr(struct rdma_session_context *rdma_session, struct ib_wc *wc){
	int ret;
	int i;		// Some local index

	if ( wc->byte_len != sizeof(struct message) ) {         // Check the length of received message
		printk(KERN_ERR "%s, Received bogus data, size %d\n", __func__,  wc->byte_len);
		return -1;
	}	

	#ifdef DEBUG_RDMA_CLIENT
	// Is this check necessary ??
	if (unlikely(rdma_session->state < CONNECTED) ) {
		printk(KERN_ERR "%s, RDMA is not connected\n", __func__);	
		return -1;
	}
	#endif


	#ifdef DEBUG_RDMA_CLIENT
	printk("%s, Recieved RDMA message: %s \n",__func__, rdma_message_print(rdma_session->recv_buf->type));
	#endif

	switch(rdma_session->recv_buf->type){
		case FREE_SIZE:
			//
			// Step 1), get the Free Regions. 
			//
			#ifdef DEBUG_RDMA_CLIENT
			printk( "%s, avaible size : %d GB \n ", __func__,	rdma_session->recv_buf->size_gb );
			#endif

			rdma_session->remote_chunk_list.remote_free_size_gb = rdma_session->recv_buf->size_gb;
			rdma_session->state = FREE_MEM_RECV;	
			
			ret = init_remote_chunk_list(rdma_session);
			if(unlikely(ret)){
				printk(KERN_ERR "Initialize the remote chunk failed. \n");
			}

			// Step 1) finished.
			wake_up_interruptible(&rdma_session->sem);

			break;
		case GOT_CHUNKS:
		//	cb->IS_sess->cb_state_list[cb->cb_index] = CB_MAPPED;
			//rdma_session->state = WAIT_OPS;
			//IS_chunk_list_init(cb);

			// Got memory chunks from remote memory server, do contiguous mapping.


			bind_remote_memory_chunks(rdma_session);

			// Received free chunks from remote memory,
			// Wakeup the waiting main thread and continure.
			rdma_session->state = RECEIVED_CHUNKS;

			#ifdef DEBUG_LATENCY_CLIENT
			printk("%s, Got all the remote memory regions from remote memory pool. Start count cycles. \n",__func__);
			#endif

			wake_up_interruptible(&rdma_session->sem);  // Finish main function.

			break;
		case GOT_SINGLE_CHUNK:
		//	cb->IS_sess->cb_state_list[cb->cb_index] = CB_MAPPED;
			//rdma_session->state = WAIT_OPS;
		

			#ifdef DEBUG_RDMA_CLIENT 
			// Check the received data
			// All the rkey[] are reset to 0 before sending to client.
			for(i=0; i< MAX_REMOTE_MEMORY_SIZE_GB/CHUNK_SIZE_GB; i++){
				if(rdma_session->recv_buf->rkey[i]){
					printk("%s, received remote chunk[%d] addr : 0x%llx, rkey : 0x%x \n", __func__, i,
																		ntohll(rdma_session->recv_buf->buf[i]), 
																		ntohl(rdma_session->recv_buf->rkey[i]));
				}
			}
			#endif

			// debug
			//rdma_session->state = TEST_DONE;
			//wake_up_interruptible(&rdma_session->sem);  // Finish main function.

			bind_remote_memory_chunks(rdma_session);

			break;
		case EVICT:
			rdma_session->state = RECV_EVICT;

			//client_recv_evict(cb);
			break;
		case STOP:
			rdma_session->state = RECV_STOP;	
		
			//client_recv_stop(cb);
			break;
		default:
			printk(KERN_ERR "%s, Recieved RDMA message UN-KNOWN \n",__func__);
			return -1; 	
	}
	return 0;
}




/**
 *  Post a 2-sided request for chunk mapping.
 * 
 * 
 * 
 * More Explanation
 * 	We can only post a cqe notification per time, OR these cqe notifications may overlap and lost.
 * 	So we post the cqe notification in cq_event_handler function.
 * 
 */
int octupos_requset_for_chunk(struct rdma_session_context* rdma_session, int num_chunk){
	int ret = 0;
	// Prepare the receive WR
	struct ib_recv_wr *bad_wr;

	if(num_chunk == 0 || rdma_session == NULL)
		goto err;

	ret = ib_post_recv(rdma_session->qp, &rdma_session->rq_wr, &bad_wr);
	if(ret) {
		printk(KERN_ERR "%s, Post 2-sided message to receive data failed.\n", __func__);
		goto err;
	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printk("%s: 2-sided RDMA message[%llu] recv. \n",__func__,rmda_ops_count++);
	}
	#endif

	// Post the send WR
//	ret = send_message_to_remote(rdma_session, num_chunk == 1 ? REQUEST_SINGLE_CHUNK : REQUEST_CHUNKS, num_chunk * CHUNK_SIZE_GB );
	ret = send_message_to_remote(rdma_session, REQUEST_CHUNKS, num_chunk * CHUNK_SIZE_GB );
	if(ret) {
		printk(KERN_ERR "%s, Post 2-sided message to remote server failed.\n", __func__);
		goto err;
	}


	return ret;

	err:
	printk(KERN_ERR "Error in %s \n", __func__);
	return ret;
}




//
// <<<<<<<<<<<<<<  End of handling TWO-SIDED RDMA message section <<<<<<<<<<<<<<
//







//
// >>>>>>>>>>>>>>>  Start of ONE-SIDED RDMA message section >>>>>>>>>>>>>>>
//	Stateful,
//		Need to allocate and maintain some resource.
//		i.e. The 1-sided WR.
//		All the reource stored in rmem_rdma_queue, mapped to each dispatch queue.
//		These resources will be invoked by the Disk Driver via the definination of blk_mq_ops->.queue_rq.
//
//  [?] Need a daemon thread ? 
//		Seems no. After getting the chunk remote_addr/rkey, no need to wait any signals.
//		The action is triggered by blk_mq_ops->.queue_rq, which pops i/o requset.
//


/**
 * [Discarded] Allocate and intialize enough WR and DMA buffers used for 1-sided RDMA messages.
 * 
 * [x] For this design, when transffer i/o request to RDMA message, one more cpu copy is needed:
 * 		Store  : need to copy all the data from physical page to DMA buffer. async, not latency sen
 * 		Load, read/write page fault : Copy data from DMA buffer to physical pages.
 * 
 * 	=> 1) We register the physical pages attached to each bio as RDMA directly, no need to this data copy.
 * 	=> 2) We use the space reserved after i/o request as rmem_rdma_command, no need to keep such a rdma_cmd pool
 * 
*/


int init_rdma_command_list(struct rdma_session_context *rdma_session, uint32_t queue_ind){
	int ret = 0;
	uint32_t i;
	struct rmem_rdma_queue		*rdma_q_ptr;
	struct rmem_rdma_command 	*rdma_cmd_ptr;  // rmem_rdma_command iterator.


	rdma_q_ptr = &(rdma_session->rmem_rdma_queue_list[queue_ind]);
	rdma_q_ptr->cmd_ptr	= 0;  												// Start from 0.
	rdma_q_ptr->rdma_queue_depth	= RMEM_QUEUE_DEPTH;	// This value should match "rmem_dev_ctrl->queue_depth" 
	rdma_q_ptr->q_index	= 0;				// The corresponding dispatch queue index, assigned in blk_mq_ops->.init_hctx.
	rdma_q_ptr->rdma_session = rdma_session;
	// Build the rdma_command list.
	rdma_q_ptr->rmem_rdma_cmd_list = (struct rmem_rdma_command*)kzalloc( sizeof(struct rmem_rdma_command) * rdma_q_ptr->rdma_queue_depth, GFP_KERNEL );

	//initialize the spinlock
	spin_lock_init(&(rdma_q_ptr->rdma_queue_lock));

	/*
	 * [!!] Delete the rmem_rdma_command pool.
   *
	 * 
	// Initialize each rmem_rdma_command
	for(i=0; i< rdma_q_ptr->rdma_queue_depth; i++){
		rdma_cmd_ptr = &(rdma_q_ptr->rmem_rdma_cmd_list[i]);


		// Set its flag
		//atomic_set(&(rdma_cmd_ptr->free_state), 1);
		rdma_cmd_ptr->free_state = 1;

		// [x] the I/O request can contain mulplte bios, each bio can have multiple segments.
		// Can't dertermine the good number for the rdma_buf size now. 
		// 64 pages per rdma_buf are enough now.
		rdma_cmd_ptr->rdma_buf = (char*)kzalloc(ONE_SIEDED_RDMA_BUF_SIZE, GFP_KERNEL); 
		rdma_cmd_ptr->rdma_dma_addr = ib_dma_map_single(rdma_session->pd->device,
																			rdma_q_ptr->rmem_rdma_cmd_list[i].rdma_buf,
																			ONE_SIEDED_RDMA_BUF_SIZE,
																			DMA_BIDIRECTIONAL);
		// [x]Latter, we can change to use the MACRO for space efficiency
		// pci_unmap_addr_set(ctx, rdma_mapping, rdma_q_ptr->rmem_rdma_cmd_list[i]->rdma_dma_addr);

		rdma_cmd_ptr->rdma_sgl.addr = rdma_cmd_ptr->rdma_dma_addr;
		#ifdef	DEBUG_RDMA_CLIENT 
		if(rdma_session->qp->device->local_dma_lkey){
			rdma_cmd_ptr->rdma_sgl.lkey = rdma_session->qp->device->local_dma_lkey;
		
			#ifdef DEBUG_RDMA_CLIENT_DETAIL
			printk("%s, rdma_queue[%d] : rdma_cmd[%d], Get lkey from dma_session->qp->device->local_dma_lkey, 0x%x. \n", __func__, 
																						queue_ind,i, rdma_cmd_ptr->rdma_sgl.lkey);
			#endif
					
		}else{
			#ifdef DEBUG_RDMA_CLIENT_DETAIL
			printk("%s, rdma_queue[%d] : rdma_cmd[%d], dma_session->qp->device->local_dma_lkey is 0 (NULL). \n", __func__, 
																						queue_ind, i);
			#endif
			rdma_cmd_ptr->rdma_sgl.lkey = 0;
		}
		#else
		rdma_cmd_ptr->rdma_sgl.lkey = rdma_session->qp->device->local_dma_lkey;
		#endif
		
		rdma_cmd_ptr->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED; // 1-sided RDMA message ? both read /write
		rdma_cmd_ptr->rdma_sq_wr.wr.sg_list = &(rdma_cmd_ptr->rdma_sgl);
		
		// An I/O request can have multiple bio, and each bio can have multiple segments, which is logial sector ?
		// But we can copy the segments from i/o request to DMA buffer and compact them together. 
		rdma_cmd_ptr->rdma_sq_wr.wr.num_sge	= 1; 

		// ib_rdma_wr->ib_send_wr->wr_id is the driver_data.
		// Seem that wc->wr_id is gotten from ib_send_wr.wr_id.
		rdma_cmd_ptr->rdma_sq_wr.wr.wr_id	= (u64)rdma_cmd_ptr; 

		// queue_rq use this field to connect to a request
		rdma_cmd_ptr->io_rq = NULL;


		// intialize some debug fileds
		#ifdef DEBUG_LATENCY_CLIENT
		rdma_cmd_ptr->rdma_q_ptr	=	rdma_q_ptr;
		#endif


	} // end of for loop.
		*/


	return ret;

err:
	printk(KERN_ERR "ERROR in %s \n",__func__);
	return ret;
}


/**
 * RDMA queue is connected to Dispatch queue.
 * All the i/o request in Dispatch queue will be transfered to a 1-sided RDMA wr stored in RDMA queue.
 */
int init_rdma_command_queue_list(struct rdma_session_context *rdma_session){

	int ret = 0;
	uint32_t i;
	//uint32_t rdma_q_num		= rdma_session->rmem_dev_ctrl->nr_queues;  // NOT build the disk driver now.
	uint32_t	rdma_q_num	=	 online_cores;

	// #ifdef DEBUG_RDMA_CLIENT
	// if(rdma_session->rmem_dev_ctrl == NULL)
	// 	printk("%s, rmem_dev_ctrl is NULL. ERROR! \n",__func__);
	// #endif

	// One rdma_command_list per dispatch queue.
	// One rdma_command per I/O request.
	rdma_session->rmem_rdma_queue_list = (struct rmem_rdma_queue *)kzalloc( sizeof(struct rmem_rdma_queue)*rdma_q_num, GFP_KERNEL );


	for(i=0; i<rdma_q_num; i++  ){
		init_rdma_command_list(rdma_session, i);  // Initialize the ith rdma_queue.
	}


	#ifdef DEBUG_RDMA_CLIENT
	printk("%s, Intialize rdma_session_context->rmem_rdma_queue_list done. \n",__func__);
	#endif


	return ret;

err:
	printk( KERN_ERR "ERROR in %s \n",__func__);
	return ret;
}


/**
 * Gather the I/O request data and build the 1-sided RDMA write
 *  Invoked by Block Deivce part. Don't declare it as static.
 * 
 * 
 */
int post_rdma_write(struct rdma_session_context *rdma_session, struct request* io_rq, struct rmem_rdma_queue* rdma_q_ptr,  
					struct remote_mapping_chunk *remote_chunk_ptr, uint64_t offset_within_chunk, uint64_t len ){

	int ret = 0;
	//int cpu;
	struct rmem_rdma_command 	*rdma_cmd_ptr = blk_mq_rq_to_pdu(io_rq);
	struct ib_send_wr 				*bad_wr;

	#ifdef DEBUG_RDMA_CLIENT
  if(remote_chunk_ptr->chunk_state != MAPPED){
    ret =-1;
    printk("%s: chunk,rkey (0x%x) isn't mapped to remote memory serveer. \n", __func__, remote_chunk_ptr->remote_rkey);
    //goto err;

		return ret;
  }
	#endif 


	// Register the physical pages attached to i/o requset as RDMA mr directly to save one more data copy.
	 ret = build_rdma_wr( rdma_q_ptr, rdma_cmd_ptr, io_rq, remote_chunk_ptr, offset_within_chunk, len);
	 if(ret == 0)
	 	goto err;


	//post the 1-sided RDMA write
	// Use the global RDMA context, rdma_session_global
	ret = ib_post_send(rdma_session->qp, (struct ib_send_wr*)&rdma_cmd_ptr->rdma_sq_wr, &bad_wr );
	if(unlikely(ret)){
		printk(KERN_ERR "%s, post 1-sided RDMA write failed. \n", __func__);
		goto err;
	}
	

	// print debug information here.
	#ifdef DEBUG_RDMA_CLIENT
		printk("%s, Post 1-sided RDMA write,  : %llu  bytes \n", __func__, len);
	#endif


	return ret;

err:
	printk(KERN_ERR "ERROR in %s \n",__func__);
	return ret;
}















/**
 * Transfer the data from bio to RDMA message
 * 1) Get dma address of the physical pages attached to bio.
 * 2) Register the dma address as RDMA mr.
 * 3) Fill the information into wr.
 * 
 * 
 * [?] The segments/sector  in the i/o request should be contiguous 
 * 
 * [?] The sector should be 4KB alignment. This can only be guaranteed in paging.
 * 
 */
int build_rdma_wr(struct rmem_rdma_queue* rdma_q_ptr, struct rmem_rdma_command *rdma_cmd_ptr, struct request * io_rq, 
									struct remote_mapping_chunk *	remote_chunk_ptr , uint64_t offse_within_chunk, uint64_t len){
	int ret = 0;
	int i;
	int dma_entry = 0;
	struct ib_device	*ibdev	=	rdma_q_ptr->rdma_session->pd->device;  // get the ib_devices
	

	// 1) Register the physical pages attached to bio to a scatterlist.
	//  Map the attached physical pages to scatter-gahter list. 
	rdma_cmd_ptr->nentry = blk_rq_map_sg(io_rq->q, io_rq, rdma_cmd_ptr->sgl );

	// Get DMA address for the entries of scatter-gather list.
	// [?] This function may merge these dma buffers into one ? they are contiguous ?
	dma_entry	=	ib_dma_map_sg( ibdev, rdma_cmd_ptr->sgl, rdma_cmd_ptr->nentry,
											rq_data_dir(io_rq) == WRITE ? DMA_TO_DEVICE : DMA_FROM_DEVICE);  // Inform PCI device the dma address of these scatterlist.
	ret = dma_entry;

	#ifdef DEBUG_RDMA_CLIENT
		if( unlikely(dma_entry == 0) ){
			printk(KERN_ERR "%s, Registered 0 entries to rdma scatterlist \n", __func__);
			return -1;
		}

	//	print_io_request_physical_pages(io_rq, __func__);
	//	print_scatterlist_info(rdma_cmd_ptr->sgl, rdma_cmd_ptr->nentry);
	#endif


	// 2) Register the DMA area as RDMA MR
	// Assign the local RDMA buffer informaton to ib_rdma_wr->sg_list directly, no need to resiger the local rdma mr ?
	//
	// ret = ib_map_mr_sg(rdma_cmd_ptr->mr, rdma_cmd_ptr->sgl, rdma_cmd_ptr->nentry, NULL, PAGE_SIZE);
	// if(ret < rdma_cmd_ptr->nentry ){
	// 	printk(KERN_ERR "%s, register DMA area as MR failed. \n", __func__);
	// 	goto err;
	// }


	// 3) fill the wr
	// Re write some field of the ib_rdma_wr
	rdma_cmd_ptr->io_rq										= io_rq;						// Reserve this i/o request as responds request. 
	rdma_cmd_ptr->rdma_sq_wr.rkey					= remote_chunk_ptr->remote_rkey;
	rdma_cmd_ptr->rdma_sq_wr.remote_addr	= remote_chunk_ptr->remote_addr + offse_within_chunk; // only read a page
	rdma_cmd_ptr->rdma_sq_wr.wr.opcode		= (rq_data_dir(io_rq) == WRITE ? IB_WR_RDMA_WRITE : IB_WR_RDMA_READ);
	rdma_cmd_ptr->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED; // 1-sided RDMA message ? both read /write
	rdma_cmd_ptr->rdma_sq_wr.wr.wr_id	= (u64)rdma_cmd_ptr;


		// 3)  one or multiple DMA areas,
		// Need to use the scatter & gather characteristics of IB.
		// We need to confirm that all the sectors are contiguous or we have to split the bio into multiple ib_rdma_wr.
		
		//
		// !! Fix this !!
		// We can limit the max number of segments in each bio by setting parameter,  request_queue->limits.max_segments
		// The support max scatter-gather numbers is limited by InfiniBand hardware.
	if(dma_entry >= MAX_REQUEST_SGL){
		printk(KERN_ERR "%s : Too many segments in this i/o request. Limit and reset the number to %d \n", __func__, MAX_REQUEST_SGL);
		dma_entry = MAX_REQUEST_SGL - 2;   // 32 leads to error, reserver 2 slots.
	}


	// [Warning] assume all the sectors in this bio is contiguous.
	// build multiple ib_sge
	//struct ib_sge sge_list[dma_entry];
	for(i=0; i<dma_entry; i++ ){
		rdma_cmd_ptr->sge_list[i].addr 		= sg_dma_address(&(rdma_cmd_ptr->sgl[i]));
		rdma_cmd_ptr->sge_list[i].length	=	sg_dma_len(&(rdma_cmd_ptr->sgl[i]));
		rdma_cmd_ptr->sge_list[i].lkey		=	rdma_q_ptr->rdma_session->qp->device->local_dma_lkey;
	}

	rdma_cmd_ptr->rdma_sq_wr.wr.sg_list		= rdma_cmd_ptr->sge_list;  // let wr.sg_list points to the start of the ib_sge array?
	rdma_cmd_ptr->rdma_sq_wr.wr.num_sge		= dma_entry;
		
		
	// !! TO DO !!
	// confirm all the sectors are contiguous 
	// Checked from the bio and the merging policy of request, all the sectors should be contiguous.
	#ifdef DEBUG_RDMA_CLIENT
		if(rq_data_dir(io_rq) == WRITE ){
			printk(KERN_INFO "%s, gather. 1-sided RDMA  write for %d segmetns.", __func__, dma_entry);
		}else{
			printk(KERN_INFO "%s, gather. 1-sided RDMA  read for %d segmetns.", __func__, dma_entry);
		}

		check_segment_address_of_request(io_rq);
	#endif

	// Used for counting latency
	#ifdef DEBUG_LATENCY_CLIENT
		rdma_cmd_ptr->rdma_q_ptr	=	rdma_q_ptr;
	#endif


	return ret; 
}




/**
 * 1-sided RDMA write is done.
 * 		Write data into remote memory pool successfully.
 * 		
 */
int rdma_write_done(struct ib_wc *wc){

	struct rmem_rdma_command 	*rdma_cmd_ptr;
	struct request 						*io_rq;


	//Get rdma_command from wr->wr_id
	rdma_cmd_ptr	= (struct rmem_rdma_command *)(wc->wr_id);
	if(unlikely(rdma_cmd_ptr == NULL)){
		printk(KERN_ERR "%s, get NULL rmem_rdma_command from wc->wr_id \n", __func__);
		return -1;
	}

	io_rq	= rdma_cmd_ptr->io_rq;
	
	//!!  Write data into remote memory pool successfully, no need to copy anything into the bio !!
	// Copy data to i/o request's physical pages
	// [!!] Assume there is only 1 page in rdma_buf [!!]
	//memcpy(bio_data(io_rq->bio), rdma_cmd_ptr->rdma_buf, PAGE_SIZE );

	#ifdef DEBUG_RDMA_CLIENT
		printk("%s, Write rquest, tag : %d finished. Return to caller. \n",__func__, rdma_cmd_ptr->io_rq->tag);
	
		print_io_request_physical_pages(io_rq, __func__);

		// struct bio * bio_ptr = rdma_cmd_ptr->io_rq->bio;
		// struct bio_vec *bv;
		// int i;


		// bio_for_each_segment_all(bv, bio_ptr, i) {
    //   struct page *page = bv->bv_page;
    //   printk("%s:  handle struct page:0x%llx  << \n", __func__, (u64)page );
 		// }
	#endif

	// Notify the caller that the i/o request is finished.
	// blk_mq_complete_request(rdma_cmd_ptr->io_rq, rdma_cmd_ptr->io_rq->errors); // meaning of parameters, error = 0?

	blk_mq_end_request(io_rq,io_rq->errors);

	// End the request handling
	#ifdef DEBUG_LATENCY_CLIENT 


	asm volatile("RDTSCP\n\t"
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t"
              "xorl %%eax, %%eax\n\t"
              "CPUID\n\t"
              : "=r"(cycles_high_after[rdma_cmd_ptr->rdma_q_ptr->q_index]), 
							"=r"(cycles_low_after[rdma_cmd_ptr->rdma_q_ptr->q_index])::"%rax", "%rbx", "%rcx",
                "%rdx");
	

	uint64_t start, end;
	start = (((uint64_t)cycles_high_before[rdma_cmd_ptr->rdma_q_ptr->q_index] << 32) | cycles_low_before[rdma_cmd_ptr->rdma_q_ptr->q_index]);
	end = (((uint64_t)cycles_high_after[rdma_cmd_ptr->rdma_q_ptr->q_index] << 32) | cycles_low_after[rdma_cmd_ptr->rdma_q_ptr->q_index]);
	printk(KERN_INFO "1-sided RDMA write lat = %llu cycles, for %d pages \n", (end - start) , io_rq->nr_phys_segments );


	#endif


	//free this rdma_command
	//free_a_rdma_cmd_to_rdma_q(rdma_cmd_ptr);

	return 0;
}






/**
 * Transfer I/O read request to 1-sided RDMA read.
 * 
 * 	For the i/o read request, it read data from file page and copy it into physical page. 
 * 
 */
int post_rdma_read(struct rdma_session_context *rdma_session, struct request* io_rq, struct rmem_rdma_queue* rdma_q_ptr,  
					struct remote_mapping_chunk *remote_chunk_ptr, uint64_t offset_within_chunk, uint64_t len ){

	int ret = 0;
	int i;
	int dma_entry = 0;
	struct rmem_rdma_command 	*rdma_cmd_ptr	= blk_mq_rq_to_pdu(io_rq);  // Convert.
	// flexible array, sgl,points to the next memory automatically.
	// rdma_cmd_ptr->sgl					=  rdma_cmd_ptr + sizeof(struct rmem_rdma_command);  
	struct ib_send_wr 				*bad_wr;

	// Confirm this need chunk is mapped. We map all the remote memory at the building of RDMA connection.
	#ifdef DEBUG_RDMA_CLIENT
  	if(unlikely(remote_chunk_ptr->chunk_state != MAPPED)){
    	printk("%s, Current chunk(rkey 0x%x) isn't mapped to remote memory serveer. \n", __func__, remote_chunk_ptr->remote_rkey);
    	return -1;
  	}
	#endif

	// Initialize the reserved space behind i/o request to struct rmem_rdma_command.
	ret = build_rdma_wr( rdma_q_ptr, rdma_cmd_ptr, io_rq, remote_chunk_ptr, offset_within_chunk, len);
	if(ret == 0)
	  goto err;


	//post the 1-sided RDMA write
	// Use the global RDMA context, rdma_session_global
	ret = ib_post_send(rdma_session->qp, (struct ib_send_wr*)&rdma_cmd_ptr->rdma_sq_wr, &bad_wr);

	#ifdef DEBUG_RDMA_CLIENT
		if(unlikely(ret)){
			printk(KERN_ERR "%s, post 1-sided RDMA read failed, return value :%d \n", __func__, ret);
			return -1;
		}

		printk("%s,Post a 1-sided RDMA read done.\n", __func__);
	#endif


	return ret;

err:
	printk(KERN_ERR " ERROR in %s. \n",__func__);
	return ret;
}



/**
 * 1-sided RDMA read is done.
 * Read data back from the remote memory server.
 * Put data back to I/O request and send it back to upper layer.
 */
int rdma_read_done(struct ib_wc *wc){
	int ret = 0;
  struct rmem_rdma_command 	*rdma_cmd_ptr;
  struct request 						*io_rq;
	//u64 received_byte_len	= 0;  // For debug.


  // Get rdma_command  attached to wr->wr_id
	// Reuse the rmem_rdam_command instance.
  rdma_cmd_ptr	= (struct rmem_rdma_command *)(wc->wr_id);
  if(unlikely(rdma_cmd_ptr == NULL)){
    printk(KERN_ERR "%s, get NULL rmem_rdma_command from wc->wr_id \n", __func__);
		ret = -1;
		goto err;
  }

	// 2) Notify the caller that the i/o request is finished.

	// Get io_request from the received RDMA message.
  io_rq	= rdma_cmd_ptr->io_rq;

	#ifdef DEBUG_RDMA_CLIENT
		//printk("%s: Should copy 0x%x bytes from RDMA buffer to request. \n",__func__,  blk_rq_bytes(io_rq));
	
		// Debug part
		// We need to confirm that all the requested data are sent back. 	
		// All the physical pages are filled with the right data.
		if( io_rq->nr_phys_segments  != io_rq->bio->bi_phys_segments ){
			printk(KERN_ERR "%s: not only one bio in this requset.  Leave out some bio ! \n",__func__);
			ret = -1;
			goto err;
		}

		// Check the data length
		// for swap bio, each segments should be exactly a page, 4K.
		check_sector_and_page_size(io_rq, __func__);


		struct bio * bio_ptr = rdma_cmd_ptr->io_rq->bio;
		struct bio_vec *bv;
		int i;


		bio_for_each_segment_all(bv, bio_ptr, i) {
  		struct page *page = bv->bv_page;
    	printk("%s:  handle struct page:0x%llx , physical page: 0x%llx  << \n", __func__, (u64)page, (u64)page_to_phys(page) );
  	}

		printk("%s: 1-sided rdma_read finished. requset->tag : %d <<<<<  \n\n",__func__, io_rq->tag);
	#endif


	//blk_mq_complete_request(io_rq, io_rq->errors); // May cause errors. 
	blk_mq_end_request(io_rq,io_rq->errors);

	// End the request handling
	#ifdef DEBUG_LATENCY_CLIENT 


	asm volatile("RDTSCP\n\t"
              "mov %%edx, %0\n\t"
              "mov %%eax, %1\n\t"
              "xorl %%eax, %%eax\n\t"
              "CPUID\n\t"
              : "=r"(cycles_high_after[rdma_cmd_ptr->rdma_q_ptr->q_index]), 
							"=r"(cycles_low_after[rdma_cmd_ptr->rdma_q_ptr->q_index])::"%rax", "%rbx", "%rcx",
                "%rdx");
	

	uint64_t start, end;
	start = (((uint64_t)cycles_high_before[rdma_cmd_ptr->rdma_q_ptr->q_index] << 32) | cycles_low_before[rdma_cmd_ptr->rdma_q_ptr->q_index]);
	end = (((uint64_t)cycles_high_after[rdma_cmd_ptr->rdma_q_ptr->q_index] << 32) | cycles_low_after[rdma_cmd_ptr->rdma_q_ptr->q_index]);
	printk(KERN_INFO "1-sided RDMA read lat = %llu cycles, for %d pages \n", (end - start) , io_rq->nr_phys_segments );

	#endif


	// 3) Resource free
	//free_a_rdma_cmd_to_rdma_q(rdma_cmd_ptr);

err:
	return ret;
}





//
// <<<<<<<<<<<<<<  End of ONE-SIDED RDMA message section <<<<<<<<<<<<<<
//




//
// >>>>>>>>>>>>>>>  Start of handling chunk management >>>>>>>>>>>>>>>
//
// After building the RDMA connection, build the Client-Chunk Remote-Chunk mapping information.
//



/**
 * Invoke this information after getting the free size of remote memory pool.
 * Initialize the chunk_list based on the chunk size and remote free memory size.
 * 
 *  
 * More Explanation:
 * 	Record the address of the mapped chunk:
 * 		remote_rkey : Used by the client, read/write data here.
 * 		remote_addr : The actual virtual address of the mapped chunk ?
 * 
 */
int init_remote_chunk_list(struct rdma_session_context *rdma_session ){

	int ret = 0;
	uint32_t i;
	uint32_t tmp_chunk_num; 
	
	// 1) initialize chunk related variables
	//CHUNK_INDEX_OFSSET = GB_OFFSET + power_of_2(CHUNK_SIZE_GB);
	tmp_chunk_num = rdma_session->remote_chunk_list.remote_free_size_gb/CHUNK_SIZE_GB; // number of chunks in remote memory.
	rdma_session->remote_chunk_list.chunk_ptr = 0;	// Points to the first empty chunk.


	rdma_session->remote_chunk_list.chunk_num = tmp_chunk_num;
	rdma_session->remote_chunk_list.remote_chunk = (struct remote_mapping_chunk*)kzalloc(sizeof(struct remote_mapping_chunk) * tmp_chunk_num, GFP_KERNEL);

	for(i=0; i < tmp_chunk_num; i++){
		rdma_session->remote_chunk_list.remote_chunk[i].chunk_state = EMPTY;
		rdma_session->remote_chunk_list.remote_chunk[i].remote_addr = 0x0;
		rdma_session->remote_chunk_list.remote_chunk[i].remote_rkey = 0x0;
	}

	return ret;
}




/**
 * Get a chunk mapping 2-sided RDMA message.
 * Bind these chunks to the cient in order.
 * 
 *	1) The information of Chunks to be bound,  is stored in the recv WR associated DMA buffer.
 * Record the address of the mapped chunk:
 * 		remote_rkey : Used by the client, read/write data here.
 * 		remote_addr : The actual virtual address of the mapped chunk
 * 
 *	2) Attach the received chunks to the rdma_session_context->remote_mapping_chunk_list->remote_mapping_chunk[]
 */
void bind_remote_memory_chunks(struct rdma_session_context *rdma_session ){

	int i; 
	uint32_t *chunk_ptr;

	chunk_ptr = &(rdma_session->remote_chunk_list.chunk_ptr);
	// Traverse the receive WR to find all the got chunks.
	for(i = 0; i < MAX_REMOTE_MEMORY_SIZE_GB/CHUNK_SIZE_GB; i++ ){
		
		#ifdef DEBUG_RDMA_CLIENT
		if( *chunk_ptr >= rdma_session->remote_chunk_list.chunk_num){
			printk(KERN_ERR "%s, Get too many chunks. \n", __func__);
			break;
		}
		#endif
		
		if(rdma_session->recv_buf->rkey[i]){
			// Sent chunk, attach to current chunk_list's tail.
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_rkey = rdma_session->recv_buf->rkey[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_addr = rdma_session->recv_buf->buf[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].chunk_state = MAPPED;
			

			#ifdef DEBUG_RDMA_CLIENT
			printk("Got chunk[%d] : remote_addr : 0x%llx, remote_rkey: 0x%x \n", i, rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_addr,
																						rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_rkey);
			#endif

			(*chunk_ptr)++;
		}

	} // for

	rdma_session->remote_chunk_list.chunk_num	= *chunk_ptr;  // Record the number of received chunks.

}





//
// <<<<<<<<<<<<<<<<<<<<<  End of handling chunk management <<<<<<<<<<<<<<<<<<<<<
//







//
// >>>>>>>>>>>>>>>  Start of fields intialization >>>>>>>>>>>>>>>
//





//int main(int argc, char* argv[]){


/**
 * Build the RDMA connection to remote memory server.
 * 	
 * Parameters
 * 		rdma_session, RDMA controller/context.
 * 			
 * 
 * More Exlanation:
 * 		[?] This function is too big, it's better to cut it into several pieces.
 * 
 */
int octopus_RDMA_connect(struct rdma_session_context *rdma_session){

	int ret;
//	struct rdma_session_context *rdma_session;
	char 	ip[] = "10.0.0.2";
	struct ib_recv_wr *bad_wr;

	//
 	// 1) init rdma_session_context
	// 		rdma_session points a the global vatiable , rdma_seesion_global.
	//		Initialize its fileds directly.

	//*rdma_session_ptr 	= (struct rdma_session_context *)kzalloc(sizeof(struct rdma_session_context), GFP_KERNEL);
	//rdma_session		= *rdma_session_ptr;


	
	// Register the IB device,
	// Parameters
	// device 				 : init_net is an external symbols of kernel, get it from module.sysvers 
	// rdma_cm_event_handler : Register a CM event handler function. Used for RDMA connection.
	// IB driver_data   	 : rdma_session_context
	// RDMA connect type 	 : IB_QPT_RC, reliable communication.
  rdma_session->cm_id = rdma_create_id(&init_net, octopus_rdma_cm_event_handler, rdma_session, RDMA_PS_TCP, IB_QPT_RC);  // TCP, RC, reliable IB connection 
  	
	// Used for a async   
	rdma_session->state = IDLE;

  // The number of  on-the-fly wr ?? every cores handle one ?? 
	// This depth is used for CQ entry ? send/receive queue depth ?
  rdma_session->txdepth = RDMA_READ_WRITE_QUEUE_DEPTH * num_online_cpus() + 1; //[?] What's this used for ? What's meaning of the value ?
	rdma_session->freed = 0;	// Flag of functions' called number. 

  // Setup socket information
	// Debug : for test, write the ip:port as 10.0.0.2:9400
  rdma_session->port = htons((uint16_t)9400);  // After transffer to big endian, the decimal value is 47140
  ret= in4_pton(ip, strlen(ip), rdma_session->addr, -1, NULL);   // char* to ipv4 address ?
  if(ret == 0){  		// kernel 4.11.0 , success 1; failed 0.
		printk("Assign ip %s to  rdma_session->addr : %s failed.\n",ip, rdma_session->addr );
	}
	rdma_session->addr_type = AF_INET;  //ipv4


  	// Initialize the queue.
  init_waitqueue_head(&rdma_session->sem);
	#ifdef DEBUG_RDMA_CLIENT
  	printk("%s, Initialized rdma_session_context fields successfully.\n", __func__);
	#endif

  //2) Resolve address(ip:port) and route to destination IB. 
  ret = rdma_resolve_ip_to_ib_device(rdma_session);
	if (unlikely(ret)){
		printk (KERN_ERR "%s, bind socket error (addr or route resolve error)\n", __func__);
		return ret;
   	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
     	printk("%s,Binded to remote server successfully.\n", __func__);
   	}
	#endif

  // 3) Create the QP,CQ, PD
	//  Before we connect to remote memory server, we have to setup the rdma queues, CQ, QP.
	//	We also need to register the DMA buffer for two-sided communication and configure the Protect Domain, PD.
	//
	// Build the rdma queues.
  ret = octopus_create_rdma_queues(rdma_session, rdma_session->cm_id);
  if(unlikely(ret)){
		printk(KERN_ERR "%s, Create rdma queues failed. \n", __func__);
  }

	// 4) Register some message passing used DMA buffer.
	// 

	// 4.1) 2-sided RDMA message intialization
	//rdma_session->mem = DMA;
	// !! Do nothing for now.
  ret = octopus_setup_buffers(rdma_session);
	if(unlikely(ret)){
		printk(KERN_ERR "%s, Bind DMA buffer error\n", __func__);
	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printk("%s, Allocate and Bind DMA buffer successfully \n", __func__);
	}
	#endif

	// 4.2) 1-sided RDMA message intialization
	//
	ret = init_rdma_command_queue_list(rdma_session);
	if(unlikely(ret)){
		printk(KERN_ERR "%s, initialize 1-sided RDMA buffers error. \n",__func__);
		goto err;
	}



	// 5) Build the connection to Remote

	// After connection, send a QUERY to query and map  the available Regions in Memory server. 
	// Post a recv wr to wait for the FREE_SIZE RDMA message, sent from remote memory server.
	//
	ret = ib_post_recv(rdma_session->qp, &rdma_session->rq_wr, &bad_wr); 
	if(ret){
		printk(KERN_ERR "%s: post a 2-sided RDMA message error \n",__func__);
		goto err;
	}	
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printk("%s: 2-sided RDMA message[%llu] send. \n",__func__, rmda_ops_count++);
	}
	#endif

	// Build RDMA connection.
	ret = octopus_connect_remote_memory_server(rdma_session);
	if(ret){
		printk(KERN_ERR "%s: Connect to remote server error \n", __func__);
		goto err;
	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printk("%s, Connect to remote server successfully \n", __func__);
	}
	#endif


	// 6) Get free memory information from Remote Mmeory Server
	// [X] After building the RDMA connection, server will send its free memory to the client.
	// Post the WR to get this RDMA two-sided message.
	// When the receive WR is finished, cq_event_handler will be triggered.
	
	// a. post a receive wr before build the RDMA connection, in 5).

	// b. Request a notification (IRQ), if an event arrives on CQ entry.
	//    Used for getting the FREE_SIZE
	//	  FREE_SIZE -> MAP_CHUNK should be done in order.
	//	  This is the first notify_cq, all other notify_cq are done in cq_handler.
	ret = ib_req_notify_cq(rdma_session->cq, IB_CQ_NEXT_COMP);   
	if (ret) {
		printk(KERN_ERR "%s, ib_create_cq failed\n", __func__);
		goto err;
	}
	#ifdef DEBUG_RDMA_CLIENT 
	else{
		printk("%s: cq_notify_count : %llu \n",__func__, cq_notify_count++);
	}
	#endif

	// Sequence controll
	wait_event_interruptible( rdma_session->sem, rdma_session->state == FREE_MEM_RECV );

	// b. Post the receive WR.
	// Should post this recv WR before connect the RDMA connection ?
	//struct ib_recv_wr *bad_wr;
	//ret = ib_post_recv(rdma_session->qp, &rdma_session->rq_wr, &bad_wr); 


	//7) Post a message to Remote memory server.
	//wait_event_interruptible(rdma_session->sem, rdma_session->state == SEND_MESSAGE);
	//printk("Receive message down, wake up to send message.\n");
	//send_messaget_to_remote(rdma_session, 9, 2);  // 9 : bind_single
	//send_messaget_to_remote(cb, 6, 2);  // 6 : activity


	// Send a RDMA message to request for mapping a chunk ?
	// Parameters
	// 		rdma_session_context : driver data
	//		number of requeted chunks
	#ifdef DEBUG_RDMA_CLIENT
	printk("%s: Got free memory size from remote memory server. Request for Chunks : %llu \n",
																												__func__, MAX_REMOTE_MEMORY_SIZE_GB/CHUNK_SIZE_GB);
	#endif
	ret = octupos_requset_for_chunk(rdma_session, MAX_REMOTE_MEMORY_SIZE_GB/CHUNK_SIZE_GB);  // 8 chunk, 1 GB/chunk.
	if(unlikely(ret)){
		printk("%s, request for chunk failed.\n", __func__);
		goto err;
	}

	// Sequence controll
	wait_event_interruptible( rdma_session->sem, rdma_session->state == RECEIVED_CHUNKS );



	// SECTION 2
	// [!!] Connect to Disk Driver  [!!]
	//
	rdma_session->rmem_dev_ctrl = &rmem_dev_ctrl_global;
	rmem_dev_ctrl_global.rdma_session = rdma_session;		// [!!]Do this before intialize Block Device.
	
	#ifndef DEBUG_RDMA_ONLY
	ret =rmem_init_disk_driver(&rmem_dev_ctrl_global);
	#endif

	if(unlikely(ret)){
		printk("%s, Initialize disk driver failed.\n", __func__);
		goto err;
	}



	// SECTION 3, FINISHED.

	// [!!] Only reach here afeter got STOP_ACK signal from remote memory server.
	// Sequence controll - FINISH.
	// Be carefull, this will lead to all the local variables collected.

	// [?] Can we wait here with function : octopus_disconenct_and_collect_resource together?
	//  NO ! https://stackoverflow.com/questions/16163932/multiple-threads-can-wait-on-a-semaphore-at-same-time
	// Use  differeent semapore signal.
	//wait_event_interruptible(rdma_session->sem, rdma_session->state == CM_DISCONNECT);

	//wait_event_interruptible( rdma_session->sem, rdma_session->state == TEST_DONE );



	printk("%s,Exit the main() function.\n", __func__);

	return ret;

err:
	//free resource
	
	// Free the rdma_session at last. 
	// if(rdma_session != NULL)
	// 	kfree(rdma_session);

	printk(KERN_ERR "ERROR in %s \n", __func__);
	return ret;
}




/**
 * >>>>>>>>>>>>>>> Start of Resource Free Functions >>>>>>>>>>>>>>>
 * 
 * For kernel space, there is no concept of multi-processes. 
 * There is only multilple kernel threads which share the same kernel virtual memory address.
 * So it's the thread's resposibility to free the allocated memory in the kernel heap.
 * 
 * 
 * 
 */



/**
 * Free the RDMA buffers.
 * 
 * 	1) 2-sided DMA buffer
 * 	2) mapped remote chunks.
 * 
 */
void octopus_free_buffers(struct rdma_session_context *rdma_session) {

	u32 i,j;
	u32 rdma_q_num = online_cores;
	struct rmem_rdma_queue		*rdma_q_ptr;
	struct rmem_rdma_command	*rdma_cmd_ptr;

	// Free the DMA buffer for 2-sided RDMA messages
	if(rdma_session == NULL)
		return;

	// Free 1-sided dma buffers
	if(rdma_session->recv_buf != NULL)
		kfree(rdma_session->recv_buf);
	if(rdma_session->send_buf != NULL)
		kfree(rdma_session->send_buf);

	// Free the 2-sided dma buffers
	// 	Level #1, free the rmem_rdma_queue
	if(rdma_session->rmem_rdma_queue_list != NULL ){
		for(i=0; i< rdma_q_num; i++){
			rdma_q_ptr = &(rdma_session->rmem_rdma_queue_list[i]);

			// Level #2, free the rmem_rdma_command_list
			if(rdma_q_ptr->rmem_rdma_cmd_list != NULL ){
				for(j = 0; j < rdma_q_ptr->rdma_queue_depth; j++  ){
					rdma_cmd_ptr = &(rdma_q_ptr->rmem_rdma_cmd_list[j]);
					
					// Level #3, free the RDMA buffer for each rdma command
					// if(rdma_cmd_ptr->rdma_buf != NULL){
					// 	kfree(rdma_cmd_ptr->rdma_buf);

					// 	#ifdef DEBUG_RDMA_CLIENT_DETAIL
					// 	printk("%s: free rdma_queue[%d], rdma_cmd_buf[%d]", __func__,i,j );
					// 	#endif
					// }

				} // for - j

				kfree(rdma_q_ptr->rmem_rdma_cmd_list);
			}
			
		}// for - i

		// free rdma queue
		kfree(rdma_session->rmem_rdma_queue_list);
	}

	// free registered regions
	// But for dma_session->pd->device->get_dma_mr
	// Can't invoke ib_dereg_mr() to free it. cause null pointer crash.
	//
	//if(rdma_session->mem == DMA){
	//	ib_dereg_mr(rdma_session->dma_mr);
	//}

	// Free the remote chunk management,
	if(rdma_session->remote_chunk_list.remote_chunk != NULL)
		kfree(rdma_session->remote_chunk_list.remote_chunk);

	#ifdef DEBUG_RDMA_CLIENT
	printk("%s, Free RDMA buffers done. \n",__func__);
	#endif

}


/**
 * Free InfiniBand related structures.
 * 
 * rdma_cm_id : the main structure to maintain the IB.
 * 		
 * 
 * 
 */
void octopus_free_rdma_structure(struct rdma_session_context *rdma_session){

	if (rdma_session == NULL)
		return;

	if(rdma_session->cm_id != NULL){
		rdma_destroy_id(rdma_session->cm_id);

		#ifdef DEBUG_RDMA_CLIENT
		printk("%s, free rdma_cm_id done. \n",__func__);
		#endif
	}

	if(rdma_session->qp != NULL){
		ib_destroy_qp(rdma_session->qp);
		//rdma_destroy_qp(rdma_session->cm_id);

		#ifdef DEBUG_RDMA_CLIENT
		printk("%s, free ib_qp  done. \n",__func__);
		#endif
	}

	// 
	// Both send_cq/recb_cq should be freed in ib_destroy_qp() ?
	//
	if(rdma_session->cq != NULL){
		ib_destroy_cq(rdma_session->cq);

		#ifdef DEBUG_RDMA_CLIENT
		printk("%s, free ib_cq  done. \n",__func__);
		#endif
	}

	// Before invoke this function, free all the resource binded to pd.
	if(rdma_session->pd != NULL){
		ib_dealloc_pd(rdma_session->pd); 

		#ifdef DEBUG_RDMA_CLIENT
		printk("%s, free ib_pd  done. \n",__func__);
		#endif
	}

	#ifdef DEBUG_RDMA_CLIENT
	printk("%s, Free RDMA structures,cm_id,qp,cq,pd done. \n",__func__);
	#endif

}


/**
 * The main entry of resource free.
 * 
 * [x] 2 call site.
 * 		1) Called by client, at the end of function, octopus_rdma_client_cleanup_module().
 * 		2) Triggered by DISCONNECT CM event, in octopus_rdma_cm_event_handler()
 * 
 */
int octopus_disconenct_and_collect_resource(struct rdma_session_context *rdma_session){

	int ret = 0;

	if(unlikely(rdma_session->freed != 0)){
		// already called by some thread,
		// just return and wait.
		return 0;
	}
	rdma_session->freed++;

	// The RDMA connection maybe already disconnected.
	if(rdma_session->state != CM_DISCONNECT){
		ret = rdma_disconnect(rdma_session->cm_id);
		if(ret){
			printk(KERN_ERR "%s, RDMA disconnect failed. \n",__func__);
		}

		// wait the ack of RDMA disconnected successfully
		wait_event_interruptible(rdma_session->sem, rdma_session->state == CM_DISCONNECT); 
	}


	#ifdef DEBUG_RDMA_CLIENT
	printk("%s, RDMA disconnected, start to free resoutce. \n", __func__);
	#endif

	// Free resouces
	octopus_free_buffers(rdma_session);
	octopus_free_rdma_structure(rdma_session);

	// If not allocated by kzalloc, no need to free it.
	//kfree(rdma_session);  // Free the RDMA context.
	

	// DEBUG -- 
	// Exit the main function first.
	// wake_up_interruptible will cause context switch, 
	// Just skip the code below this invocation.
	//rdma_session_global.state = TEST_DONE;
	//wake_up_interruptible(&(rdma_session_global.sem));


	#ifdef DEBUG_RDMA_CLIENT
	printk("%s, RDMA memory resouce freed. \n", __func__);
	#endif

	return ret;
}


/**
 * <<<<<<<<<<<<<<<<<<<<< End of  Resource Free Functions <<<<<<<<<<<<<<<<<<<<<
 */




//
// Kernel Module registration functions.
//



// invoked by insmod 
int __init octopus_rdma_client_init_module(void)
{

	int ret = 0;
	//printk("Do nothing for now. \n");
	printk("%s, octopus - kernel level rdma client.. \n",__func__);

	online_cores = num_online_cpus();

	#ifdef	DEBUG_BD_ONLY

		// Only debug the Disk driver
		ret =rmem_init_disk_driver(&rmem_dev_ctrl_global);
		if(unlikely(ret)){
			printk("%s, Initialize disk driver failed.\n", __func__);
			goto err;
		}
	
	#else
		// Build both the RDMA and Disk driver
		ret = octopus_RDMA_connect(&rdma_session_global);
		if(ret){
			printk(KERN_ERR "%s, octopus_RDMA_connect failed. \n", __func__);
			goto err;
		}

	// end of DEBUG_BD_ONLY
	#endif

	return ret;

err:
	printk(KERN_ERR "ERROR in %s \n", __func__);
	return ret;
}

// invoked by rmmod 
void __exit octopus_rdma_client_cleanup_module(void)
{
  	
	int ret;
	
	printk(" Prepare for removing Kernel space IB test module - octopus .\n");

	// 
	//[?] Should send a disconnect event to remote memory server?
	//		Invoke free_qp directly will cause kernel crashed. 
	//

	//octopus_free_qp(rdma_session_global);
	//IS_free_buffers(cb);  //!! DO Not invoke this.
	//if(cb != NULL && cb->cm_id != NULL){
	//	rdma_disconnect(cb->cm_id);
	//}

	#ifdef	DEBUG_BD_ONLY

		ret = octopus_free_block_devicce(&rmem_dev_ctrl_global);
		if(unlikely(ret)){
			printk(KERN_ERR "%s, free block device failed.\n",  __func__);
		}

	#else

		ret = octopus_disconenct_and_collect_resource(&rdma_session_global);
		if(unlikely(ret)){
			printk(KERN_ERR "%s, octopus_disconenct_and_collect_resource  failed.\n",  __func__);
		}

		//
		// 2)  Free the Block Device resource
		//
		#ifndef DEBUG_RDMA_ONLY
		ret = octopus_free_block_devicce(&rmem_dev_ctrl_global);
		if(unlikely(ret)){
			printk(KERN_ERR "%s, free block device failed.\n",  __func__);
		}
		#endif

		// end ofDEBUG_BD_ONLY
	#endif
	printk(" Remove Module OCTOPUS DONE. \n");

	//
	// [!!] This cause kernel crashes, not know the reason now.
	//


	return;
}

module_init(octopus_rdma_client_init_module);
module_exit(octopus_rdma_client_cleanup_module);







/**
 * >>>>>>>>>>>>>>> Start of Debug functions >>>>>>>>>>>>>>>
 *
 */


//
// Print the RDMA Communication Message 
//
char* rdma_cm_message_print(int cm_message_id){
	char* message_type_name = (char*)kzalloc(32, GFP_KERNEL); // 32 bytes

	switch(cm_message_id){
		case 0:
			strcpy(message_type_name,  "RDMA_CM_EVENT_ADDR_RESOLVED");
			break;
		case 1:
			strcpy(message_type_name, "RDMA_CM_EVENT_ADDR_ERROR");
			break;
		case 2:
			strcpy(message_type_name, "RDMA_CM_EVENT_ROUTE_RESOLVED");
			break;
		case 3:
			strcpy(message_type_name, "RDMA_CM_EVENT_ROUTE_ERROR");
			break;
		case 4:
			strcpy(message_type_name, "RDMA_CM_EVENT_CONNECT_REQUEST");
			break;
		case 5:
			strcpy(message_type_name, "RDMA_CM_EVENT_CONNECT_RESPONSE");
			break;
		case 6:
			strcpy(message_type_name, "RDMA_CM_EVENT_CONNECT_ERROR");
			break;
		case 7:
			strcpy(message_type_name, "RDMA_CM_EVENT_UNREACHABLE");
			break;
		case 8:
			strcpy(message_type_name, "RDMA_CM_EVENT_REJECTED");
			break;
		case 9:
			strcpy(message_type_name, "RDMA_CM_EVENT_ESTABLISHED");
			break;
		case 10:
			strcpy(message_type_name, "RDMA_CM_EVENT_DISCONNECTED");
			break;
		case 11:
			strcpy(message_type_name, "RDMA_CM_EVENT_DEVICE_REMOVAL");
			break;
		case 12:
			strcpy(message_type_name, "RDMA_CM_EVENT_MULTICAST_JOIN");
			break;
		case 13:
			strcpy(message_type_name, "RDMA_CM_EVENT_MULTICAST_ERROR");
			break;
		case 14:
			strcpy(message_type_name, "RDMA_CM_EVENT_ADDR_CHANGE");
			break;
		case 15:
			strcpy(message_type_name, "RDMA_CM_EVENT_TIMEWAIT_EXIT");
			break;
		default:
			strcpy(message_type_name, "ERROR Message Type");
			break;
	}

	return message_type_name;
}


/**
 * wc.status name
 * 
 */
char* rdma_wc_status_name(int wc_status_id){
	char* message_type_name = (char*)kzalloc(32, GFP_KERNEL); // 32 bytes

	switch(wc_status_id){
		case 0:
			strcpy(message_type_name, "IB_WC_SUCCESS");
			break;
		case 1:
			strcpy(message_type_name, "IB_WC_LOC_LEN_ERR");
			break;
		case 2:
			strcpy(message_type_name, "IB_WC_LOC_QP_OP_ERR");
			break;
		case 3:
			strcpy(message_type_name, "IB_WC_LOC_EEC_OP_ERR");
			break;
		case 4:
			strcpy(message_type_name, "IB_WC_LOC_PROT_ERR");
			break;
		case 5:
			strcpy(message_type_name, "IB_WC_WR_FLUSH_ERR");
			break;
		case 6:
			strcpy(message_type_name, "IB_WC_MW_BIND_ERR");
			break;
		case 7:
			strcpy(message_type_name, "IB_WC_BAD_RESP_ERR");
			break;
		case 8:
			strcpy(message_type_name, "IB_WC_LOC_ACCESS_ERR");
			break;
		case 9:
			strcpy(message_type_name, "IB_WC_REM_INV_REQ_ERR");
			break;
		case 10:
			strcpy(message_type_name, "IB_WC_REM_ACCESS_ERR");
			break;
		case 11:
			strcpy(message_type_name, "IB_WC_REM_OP_ERR");
			break;
		case 12:
			strcpy(message_type_name, "IB_WC_RETRY_EXC_ERR");
			break;
		case 13:
			strcpy(message_type_name, "IB_WC_RNR_RETRY_EXC_ERR");
			break;
		case 14:
			strcpy(message_type_name, "IB_WC_LOC_RDD_VIOL_ERR");
			break;
		case 15:
			strcpy(message_type_name, "IB_WC_REM_INV_RD_REQ_ERR");
			break;
		case 16:
			strcpy(message_type_name, "IB_WC_REM_ABORT_ERR");
			break;
		case 17:
			strcpy(message_type_name, "IB_WC_INV_EECN_ERR");
			break;
		case 18:
			strcpy(message_type_name, "IB_WC_INV_EEC_STATE_ERR");
			break;
		case 19:
			strcpy(message_type_name, "IB_WC_FATAL_ERR");
			break;
		case 20:
			strcpy(message_type_name, "IB_WC_RESP_TIMEOUT_ERR");
			break;
		case 21:
			strcpy(message_type_name, "IB_WC_GENERAL_ERR");
			break;
		default:
			strcpy(message_type_name, "ERROR Message Type");
			break;
	}

	return message_type_name;
}



/**
 * The message type name, used for 2-sided RDMA communication.
 */
char* rdma_message_print(int message_id){
	char* message_type_name;
	message_type_name = (char*)kzalloc(32, GFP_KERNEL); // 32 bytes

	switch(message_id){

		case 1:
			strcpy(message_type_name, "DONE");
			break;

		case 2:
			strcpy(message_type_name, "GOT_CHUNKS");
			break;

		case 3:
			strcpy(message_type_name, "GOT_SINGLE_CHUNK");
			break;

		case 4:
			strcpy(message_type_name, "FREE_SIZE");
			break;

		case 5:
			strcpy(message_type_name, "EVICT");
			break;

		case 6:
			strcpy(message_type_name, "ACTIVITY");
			break;

		case 7:
			strcpy(message_type_name, "STOP");
			break;

		case 8:
			strcpy(message_type_name, "REQUEST_CHUNKS");
			break;

		case 9:
			strcpy(message_type_name, "REQUEST_SINGLE_CHUNK");
			break;

		case 10:
			strcpy(message_type_name, "QUERY");
			break;

		default:
			strcpy(message_type_name, "ERROR Message Type");
			break;
	}

	return message_type_name;
}



// Print the string of rdma_session_context state.
// void rdma_session_context_state_print(int id){
char* rdma_session_context_state_print(int id){

	char* rdma_seesion_state_name;
	rdma_seesion_state_name = (char*)kzalloc(32, GFP_KERNEL); // 32 bytes.

	switch (id){

		case 1 :
			strcpy(rdma_seesion_state_name, "IDLE");
			break;
		case 2 :
			strcpy(rdma_seesion_state_name, "CONNECT_REQUEST");
			break;
		case 3 :
			strcpy(rdma_seesion_state_name, "ADDR_RESOLVED");
			break;
		case 4 :
			strcpy(rdma_seesion_state_name, "ROUTE_RESOLVED");
			break;
		case 5 :
			strcpy(rdma_seesion_state_name, "CONNECTED");
			break;
		case 6 :
			strcpy(rdma_seesion_state_name, "FREE_MEM_RECV");
			break;
		case 7 :
			strcpy(rdma_seesion_state_name, "RECEIVED_CHUNKS");
			break;
		case 8 :
			strcpy(rdma_seesion_state_name, "RDMA_BUF_ADV");
			break;
		case 9 :
			strcpy(rdma_seesion_state_name, "WAIT_OPS");
			break;
		case 10 :
			strcpy(rdma_seesion_state_name, "RECV_STOP");
			break;
		case 11 :
			strcpy(rdma_seesion_state_name, "RECV_EVICT");
			break;
		case 12 :
			strcpy(rdma_seesion_state_name, "RDMA_WRITE_RUNNING");
			break;
		case 13 :
			strcpy(rdma_seesion_state_name, "RDMA_READ_RUNNING");
			break;
		case 14 :
			strcpy(rdma_seesion_state_name, "SEND_DONE");
			break;
		case 15 :
			strcpy(rdma_seesion_state_name, "RDMA_DONE");
			break;
		case 16 :
			strcpy(rdma_seesion_state_name, "RDMA_READ_ADV");
			break;
		case 17 :
			strcpy(rdma_seesion_state_name, "RDMA_WRITE_ADV");
			break;
		case 18 :
			strcpy(rdma_seesion_state_name, "CM_DISCONNECT");
			break;
		case 19 :
			strcpy(rdma_seesion_state_name, "ERROR");
			break;

		default :
			strcpy(rdma_seesion_state_name, "Un-defined state.");
			break;
	}

	return rdma_seesion_state_name;
}



/**
 * <<<<<<<<<<<<<<<<<<<<< End of Debug Functions <<<<<<<<<<<<<<<<<<<<<
 */








/**
 * >>>>>>>>>>>>>>> Start of STALED functions >>>>>>>>>>>>>>>
 *
 */



/**
 * [Discarded]Copy data from I/O request to RDMA buffer.
 * We avoid the data copy by register physical pages attached  to bio as RDMA buffer directly.
 */

/*
void copy_data_to_rdma_buf(struct request *io_rq, struct rmem_rdma_command *rdma_ptr){

	uint32_t 	num_seg		= io_rq->nr_phys_segments;	// 4K per segment.
	struct bio 	*bio_ptr	= io_rq->bio; 				// the head of bio list.
	uint32_t 	seg_ind;
	char		*buf;

	for(seg_ind=0; seg_ind< num_seg; ){

		// #ifdef DEBUG_RDMA_CLIENT
		// printk("%s, Copy %d pages(segment) from bio 0x%llx to 0x%llx \n",__func__, bio_ptr->bi_phys_segments,
		// 																(unsigned long long)buf, 
		// 																(unsigned long long)(rdma_ptr->rdma_buf + (seg_ind * PAGE_SIZE)) );
		// #endif

		buf = bio_data(bio_ptr);
		memcpy(rdma_ptr->rdma_buf + (seg_ind * PAGE_SIZE), buf, bio_ptr->bi_phys_segments * PAGE_SIZE  );

		// next iteration
		seg_ind+=bio_ptr->bi_phys_segments;
		bio_ptr = bio_ptr->bi_next;
	}
	

	//debug section
	#ifdef DEBUG_RDMA_CLIENT
	printk("%s, cuurent i/o write  request, 0x%llx, has [%u] pages. \n", __func__,(unsigned long long)io_rq ,num_seg);
	#endif

}

*/






	/*
	// Reserved code fo expand build_rdma_wr into post_rdma_read/write function.
	// Expand the build_rdma_wr here.
	
	int dma_entry = 0;
	int i = 0;
	struct ib_device	*ibdev	=	rdma_q_ptr->rdma_session->pd->device;  // get the ib_devices

	// 1) Register the physical pages attached to bio to a scatterlist.
	//  does the scatterlist->dma_address assigned by ib_dma_map_sg, yes.
	// Make sure that rdma_cmd_ptr->sgl has enough slots for the segments in request. 
	rdma_cmd_ptr->nentry = blk_rq_map_sg(io_rq->q, io_rq, rdma_cmd_ptr->sgl );

	// 2) Register all the entries in scatterlist as dma buffer.
	// This function may merge these dma buffers into one.
	dma_entry	=	ib_dma_map_sg( ibdev, rdma_cmd_ptr->sgl, rdma_cmd_ptr->nentry,
											DMA_TO_DEVICE);  // Inform PCI device the dma address of these scatterlist.
	ret = dma_entry;
	

	#ifdef DEBUG_RDMA_CLIENT
		if( unlikely(dma_entry == 0) ){
			printk(KERN_ERR "%s, Registered 0 entries to rdma scatterlist \n", __func__);
			return -1;
		}else{
			printk(KERN_INFO "%s, Regsitered %d entries as rdma buffer \n", __func__, dma_entry);
		}

	//	print_io_request_physical_pages(io_rq, __func__);
	//	print_scatterlist_info(rdma_cmd_ptr->sgl, rdma_cmd_ptr->nentry);
	#endif

	rdma_cmd_ptr->io_rq				= io_rq;						// Reserve this i/o request as responds request. 
	rdma_cmd_ptr->rdma_sq_wr.rkey					= remote_chunk_ptr->remote_rkey;
	rdma_cmd_ptr->rdma_sq_wr.remote_addr	= remote_chunk_ptr->remote_addr + offset_within_chunk;
	rdma_cmd_ptr->rdma_sq_wr.wr.opcode		= IB_WR_RDMA_WRITE OR IB_WR_RDMA_READ;
	rdma_cmd_ptr->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED; // 1-sided RDMA message ? both read /write
	rdma_cmd_ptr->rdma_sq_wr.wr.wr_id	= (u64)rdma_cmd_ptr;

	//
	// !! Fix this !!
	//
	if(dma_entry >= MAX_REQUEST_SGL){
		printk(KERN_ERR "%s : Too many segments in this i/o request. Limit and reset the number to %d \n", __func__, MAX_REQUEST_SGL);
		dma_entry = MAX_REQUEST_SGL - 2;  // 32 leands to error, reserve 2 slots.
	}

	struct ib_sge sge_list[dma_entry];   // This variable size length of array will disable the goto function.

	// Do we need to handle 1 and multiple dma_entries separately ?
	for(i=0; i<dma_entry; i++ ){
		sge_list[i].addr 		= sg_dma_address(&(rdma_cmd_ptr->sgl[i]));
		sge_list[i].length	=	sg_dma_len(&(rdma_cmd_ptr->sgl[i]));
		sge_list[i].lkey		=	rdma_q_ptr->rdma_session->qp->device->local_dma_lkey;
	}

	rdma_cmd_ptr->rdma_sq_wr.wr.sg_list		= sge_list;  // let wr.sg_list points to the start of the ib_sge array?
	rdma_cmd_ptr->rdma_sq_wr.wr.num_sge		= dma_entry;



	//debug
	// responds the request before post the request.
	//blk_mq_complete_request(io_rq, io_rq->errors);
	*/
	// End of expand






/**
 * [DISCARDED] Get a free wr to carry the read/write bio.
 * 
 * [?]Here may cause override problems caused by concurrency problems. get the same rdma_cmd_ind ??
 * 		Every dispatch queue can only use its own rdma_queue.
 * 		And the cpu preempt is also disabled. 
 * 		Do we also need to disable the hardware interruption ?
 * 
 * 
 * [?] Can we resuse the physical memory pages in bio as RDMA write buffer directly ?
 * 
 * 
 * https://lwn.net/Articles/695257/
 * 
 */

/*
struct rmem_rdma_command* get_a_free_rdma_cmd_from_rdma_q(struct rmem_rdma_queue* rmda_q_ptr){

	int rdma_cmd_ind  = 0;
	unsigned long flags;
	//int cpu;

	// Fast path, use the slots pointed by rmem_rdma_queue->ptr.

	// 					TO BE DONE.

	//
	// Slow path,  traverse the list to find one.
	// critical section

	while(1){

		#ifdef DEBUG_RDMA_CLIENT
		printk("%s: TRY to get rdma_queue[%d] rdma_cmd[%d].\n",__func__, rmda_q_ptr->q_index, rdma_cmd_ind);
		#endif

		//busy waiting.
		// lock 
		spin_lock_irqsave(&(rmda_q_ptr->rdma_queue_lock), flags);


		if( rmda_q_ptr->rmem_rdma_cmd_list[rdma_cmd_ind].free_state == 1 ){  // 1 means available.
			rmda_q_ptr->rmem_rdma_cmd_list[rdma_cmd_ind].free_state =0;  // [??] This can't solve the concurry prolbems.
			
			// unlock - 1)
			spin_unlock_irqrestore(&(rmda_q_ptr->rdma_queue_lock), flags);

			#ifdef DEBUG_RDMA_CLIENT
			printk("%s: SUCC rdma_queue[%d]  get rdma_cmd[%d] \n",__func__,rmda_q_ptr->q_index ,rdma_cmd_ind);
			#endif

			return &(rmda_q_ptr->rmem_rdma_cmd_list[rdma_cmd_ind]);  // move forward
		}

		if(rdma_cmd_ind++ == rmda_q_ptr->rdma_queue_depth - 1){
			rdma_cmd_ind = 0;  
		}

		// unlock - 2)
		spin_unlock_irqrestore(&(rmda_q_ptr->rdma_queue_lock), flags);

	}// while.
	//put_cpu();


//err:
	printk(KERN_ERR "%s, Current rmem_rdma_queue[%d] : runs out rmem_rdma_cmd \n",__func__, rmda_q_ptr->q_index);
	return NULL;
}

*/

// Set this rdma_command free
// Free is a concurrent safe procedure
// inline void free_a_rdma_cmd_to_rdma_q(struct rmem_rdma_command* rdma_cmd_ptr){
// 	//atomic_set(&(rdma_cmd_ptr->free_state), 1);
// 	rdma_cmd_ptr->free_state = 1;
// 	rdma_cmd_ptr->io_rq 	=	NULL;
// }








/**
 * <<<<<<<<<<<<<<<<<<<<< End of STALED Functions <<<<<<<<<<<<<<<<<<<<<
 */




