/**
 * rdma_client.c is used for translating the I/O requests to RDMA messages 
 * 	and sent them to remote memory server via InfiniBand.
 * 
 * The kernel module initializes  global variables and register stateless functions to the RDMA driver.
 * 
 * 1) Global variables
 * 		a. struct rdma_session_context		rdma_seesion_global; 
 * 			contains all the RDMA controllers, rdma_cm_id, ib_qp, ibv_cq, ib_pd etc.
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

#include "semeru_cpu_user.h"

//
// Implement the global vatiables here
//

struct rdma_session_context 	rdma_session_global;
int online_cores;

//debug
uint64_t	rmda_ops_count	= 0;
uint64_t	cq_notify_count	= 0;
uint64_t	cq_get_count		= 0;
//
// ############# Start of RDMA Communication (CM) event handler ########################
//
// [?] All the RDMA Communication(CM) event is triggered by hardware and mellanox driver.
//		No need to maintain a daemon thread to handle the CM events ?  -> stateless handler 
//


/**
 * The rdma CM event handler function
 * [X] For user space, we have to invoke rdma_get_cm_event() explicitly to get the event.
 * 
 * More Explanation
 * 	 CMA Event handler  && cq_event_handler , 2 different functions for CM event and normal RDMA message handling.
 * 
 */
//int octopus_rdma_cm_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event){
	
int octopus_rdma_cm_event_handler(struct  rdma_session_context *rdma_session ){
	int ret;
	struct rdma_cm_event 	*event;
	struct rdma_cm_id			*cma_id	=	rdma_session->cm_id;
	//struct rdma_session_context *rdma_session = cma_id->context;

	ret =  rdma_get_cm_event( rdma_session->event_channel ,&event);
	if(ret != 0)
		goto err;


	#ifdef DEBUG_RDMA_CLIENT
	printf("cma_event type %d, type_name: %s \n", event->event, rdma_cm_message_print(event->event));
	#endif

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:

		#ifdef DEBUG_RDMA_CLIENT
		// RDMA route is solved, wake up the main process  to continue.
    	printf("%s : RDMA_CM_EVENT_ADDR_RESOLVED\n ",__func__);
		#endif	

		rdma_session->state = ADDR_RESOLVED;
		// Go to next step, resolve the rdma route.
		// ret = rdma_resolve_route(cma_id, 2000);
		// if (ret) {
		// 	printf( "%s,rdma_resolve_route error %d\n", __func__, ret);
		// //	wake_up_interruptible(&rdma_session->sem);
		// }

		//After resolve the addr, ACK Memory server.
		rdma_ack_cm_event(event);
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:

		#ifdef DEBUG_RDMA_CLIENT
		// RDMA route is solved, wake up the main process  to continue.
    	printf("%s : RDMA_CM_EVENT_ROUTE_RESOLVED, wake up rdma_session->sem\n ", __func__);
		#endif	

		// Sequencial controll 
		rdma_session->state = ROUTE_RESOLVED;

		//After resolve the route, ACK Memory server.
		rdma_ack_cm_event(event);

		//wake_up_interruptible(&rdma_session->sem);
		sem_post(&rdma_session->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:		// Receive RDMA connection request
		//rdma_session->state = CONNECT_REQUEST;

    	printf("Receive but Not Handle : RDMA_CM_EVENT_CONNECT_REQUEST \n");
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
	    printf("%s, ESTABLISHED, wake up kernel_cb->sem\n", __func__);

		rdma_session->state = CONNECTED;

		//After connection established, ACK Memory server.
		rdma_ack_cm_event(event);

    //wake_up_interruptible(&rdma_session->sem);		
		sem_post(&rdma_session->sem);

		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printf( "%s, cma event %d, event name %s, error code %d \n", __func__, event->event,
														rdma_cm_message_print(event->event), event->status);
		rdma_session->state = ERROR;
		//wake_up_interruptible(&rdma_session->sem);
		sem_post(&rdma_session->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:	//should get error msg from here
		printf( "%s, Receive DISCONNECTED  signal \n",__func__);
		//rdma_session->state = CM_DISCONNECT;

		if(rdma_session->freed){ // 1, during free process.
			// Client request for RDMA disconnection.
			#ifdef DEBUG_RDMA_CLIENT
			printf("%s, RDMA disconnect evetn, requested by client. \n",__func__);
			#endif

			// Wakeup the caller.
			// wake_up_interruptible(&rdma_session->sem);



		}else{					// freed ==0, newly start free process
			// Remote server requests for disconnection.

			// !! DEAD PATH NOW --> CAN NOT FREE CM_ID !!
			// wait for RDMA_CM_EVENT_TIMEWAIT_EXIT ??
			//			TO BE DONE

			#ifdef DEBUG_RDMA_CLIENT
			printf("%s, RDMA disconnect evetn, requested by client. \n",__func__);
			#endif
			//do we need to inform the client, the connect is broken ?
			rdma_disconnect(rdma_session->cm_id);

			octopus_disconenct_and_collect_resource(rdma_session);  	// Free RDMA resource and exit main function
		//	octopus_free_block_devicce(rdma_session->rmem_dev_ctrl);	// Free block device resource 
		}
		break;

	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		// After the received DISCONNECTED_EVENT, need to wait the on-the-fly RDMA message
		// https://linux.die.net/man/3/rdma_get_cm_event

		printf("%s, Wait for in-the-fly RDMA message finished. \n",__func__);
		rdma_session->state = CM_DISCONNECT;

		//Wakeup caller
		//wake_up_interruptible(&rdma_session->sem);
		sem_post(&rdma_session->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:	//this also should be treated as disconnection, and continue disk swap
		printf( "%s, cma detected device removal!!!!\n", __func__);
		return -1;
		break;

	default:
		printf( "%s,oof bad type!\n",__func__);
		//wake_up_interruptible(&rdma_session->sem);
		sem_post(&rdma_session->sem);
		break;
	}

	return ret;

err:
	printf("Error in %s \n", __func__);
	return ret;
}




// Resolve the destination IB device by the destination IP.
// [?] Need to build some route table ?
// 
int rdma_resolve_ip_to_ib_device(struct rdma_session_context *rdma_session){
	//struct sockaddr_storage sin; 
	struct sockaddr_in addr;
	const char* port_str  = "9400";
  const char* ip_str    = "10.0.0.2";
	int ret;


	//fill_sockaddr(&sin, cb);
	// Assume that it's ipv6
	// [?]cast "struct sockaddr_storage" to "sockaddr_in" ??
	//
	// struct sockaddr_in *sin4 = (struct sockaddr_in *)&sin;   
	// sin4->sin_family = AF_INET;
	// memcpy((void *)&(sin4->sin_addr.s_addr), rdma_session->addr, 4);   	// copy 32bits/ 4bytes from cb->addr to sin4->sin_addr.s_addr
	// sin4->sin_port = rdma_session->port;                             		// assign cb->port to sin4->sin_port

	memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;                    //[?] Ipv6 is cpmpatible with Ipv4.
  inet_pton(AF_INET, ip_str, &addr.sin_addr);		// Remote memory pool is waiting on 10.0.0.2:9400.
  addr.sin_port = htons(atoi(port_str));


	ret = rdma_resolve_addr(rdma_session->cm_id, NULL, (struct sockaddr *)&addr, 2000); // timeout 2000ms
	if (ret) {
		printf( "%s, rdma_resolve_ip_to_ib_device error %d\n", __func__, ret);
		return ret;
	}else{
		printf("rdma_resolve_ip_to_ib_device - rdma_resolve_addr success.\n");
	}
	
	//
	// In user space, need to get cma event manually   
	//
	ret = octopus_rdma_cm_event_handler(rdma_session);
	if(ret != 0){
		printf("%s, octopus_rdma_cm_event_handler failed. \n",__func__);
		goto err;
	}

	ret = rdma_resolve_route(rdma_session->cm_id, 2000);
	if(ret != 0){
		printf("%s, rdma_resolve_route failed. \n",__func__);
		goto err;
	}

	ret = octopus_rdma_cm_event_handler(rdma_session);
	if(ret != 0){
		printf("%s, octopus_rdma_cm_event_handler failed. \n",__func__);
		goto err;
	}

	// Wait for the CM events to be finished:  handled by rdma_cm_event_handler()
	// 	1) resolve addr
	//	2) resolve route
	// Come back here and continue:
	//
	//wait_event_interruptible(rdma_session->sem, rdma_session->state >= ROUTE_RESOLVED);   //[?] Wait on cb->sem ?? Which process will wake up it.
	sem_wait(&rdma_session->sem);
	if (rdma_session->state != ROUTE_RESOLVED) {
		printf(  "%s, addr/route resolution did not resolve: state %d\n", __func__, rdma_session->state);
		return -EINTR;
	}
	printf("rdma_resolve_ip_to_ib_device -  resolve address and route successfully\n");
	return ret;

err:
	printf("Error in %s\n", __func__);
	return ret;
}



/**
 * Build the Queue Pair (QP).
 * 
 */
int octopus_create_qp(struct rdma_session_context *rdma_session){
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = rdma_session->txdepth; /*FIXME: You may need to tune the maximum work request */
	init_attr.cap.max_recv_wr = rdma_session->txdepth;  
	init_attr.cap.max_recv_sge = 1;					// [?] ib_sge ?  Each wr can only support one buffer ? not use the scatter/gather ?
	init_attr.cap.max_send_sge = 1;
//	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;     // Receive WR ?
	init_attr.qp_type = IBV_QPT_RC;                // Queue Pair connect type, Reliable Communication.  [?] Already assign this during create cm_id.

	// [?] Can both recv_cq and send_cq use the same cq ??
	init_attr.send_cq = rdma_session->cq;
	init_attr.recv_cq = rdma_session->cq;

	ret = rdma_create_qp(rdma_session->cm_id, rdma_session->pd, &init_attr);
	if (!ret){
		// Record this queue pair.
		rdma_session->qp = rdma_session->cm_id->qp;
  	}else{
    	printf("Create QP falied.\n");
  	}

	return ret;
}





// Prepare for building the Connection to remote IB servers. 
// Create Queue Pair : pd, cq, qp, 
int octopus_create_rdma_queues(struct rdma_session_context *rdma_session, struct rdma_cm_id *cm_id){
	int ret = 0;

	//struct ib_cq_init_attr init_attr;
	// 1) Build PD.
	// flags of Protection Domain, (ib_pd) : Protect the local OR remote memory region ? ?  
	// Local Read is default.
	rdma_session->pd = ibv_alloc_pd(cm_id->verbs);  // when creating cm_id->verbs, we pass NULL to it.

	if (rdma_session->pd  == NULL){
		printf("%s, ibv_alloc_pd failed\n", __func__);
		ret = -1;
		goto err;
	}
	printf("%s, created pd %p\n", __func__, rdma_session->pd);

	// Deliver WC to CQ. 
	rdma_session->comp_channel = ibv_create_comp_channel(cm_id->verbs);
	if(rdma_session->comp_channel == NULL ){
		printf("%s, ibv_create_comp_channel failed\n", __func__);
		ret = -1;
		goto err;
	}
	printf("%s, created communication channel %p\n", __func__, rdma_session->comp_channel);


	// 2) Build CQ
	// memset(&init_attr, 0, sizeof(init_attr));
	// init_attr.cqe = rdma_session->txdepth * 2;     // [?] The depth of cq. Number of completion queue entries.  ??
	// init_attr.comp_vector = 0;					   // [?] What's the meaning of this ??
	
	// Set up the completion queues and the cq evnet handler.
	// 
	// Parameters:
	// struct ibv_context *context, int cqe,  void *cq_context, struct ibv_comp_channel *channel, int comp_vector
	rdma_session->cq	=	ibv_create_cq(cm_id->verbs, rdma_session->txdepth * 2, NULL, rdma_session->comp_channel,  0);
	if (rdma_session->cq == 0 ) {
		printf( "%s, ibv_create_cq failed\n", __func__);
		ret = -1;
		goto err;
	}
	printf("%s, created cq %p\n", __func__, rdma_session->cq);

  // Request a notification (IRQ), if an event arrives on CQ entry.
	ret = ibv_req_notify_cq(rdma_session->cq, 0);   
	if (ret) {
	 	printf("%s, ibv_req_notify_cq failed\n", __func__);
	 	goto err;
	}

	// 3) Build QP.
	ret = octopus_create_qp(rdma_session);
	if (ret) {
		printf( "%s, failed: %d\n", __func__, ret);
		goto err;
	}
	printf("%s, created qp %p\n", __func__, rdma_session->qp);
	return ret;

err:
	//ib_dealloc_pd(cb->pd);
  	printf("Error in %s \n", __func__);
	return ret;
}





/**
 * Reserve two RDMA wr for receive/send meesages
 * 		rdma_session_context->rq_wr
 * 		rdma_session_context->send_sgl
 * Post these 2 WRs to receive/send controll messages.
 */
void octopus_setup_message_wr(struct rdma_session_context *rdma_context){
	// 1) Reserve a wr to receive RDMA message
	rdma_context->recv_sgl.addr 	= (uintptr_t)rdma_context->recv_buf;      
	rdma_context->recv_sgl.length = sizeof(struct message);
	rdma_context->recv_sgl.lkey		=	rdma_context->recv_mr->lkey;

	//rdma_context->rq_wr =	?			// Can we not initialize it ?
	rdma_context->rq_wr.sg_list = &rdma_context->recv_sgl;
	rdma_context->rq_wr.num_sge = 1;
	

	// 2) Reserve a wr
	rdma_context->send_sgl.addr = (uintptr_t)rdma_context->send_buf;
	rdma_context->send_sgl.length = sizeof(struct message);
	rdma_context->send_sgl.lkey		=	rdma_context->send_mr->lkey;

	rdma_context->sq_wr.opcode = IBV_WR_SEND;		// ib_send_wr.opcode , passed to wc.
	rdma_context->sq_wr.send_flags = IBV_SEND_SIGNALED;
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
	// 2) Allocate a DMA Memory Region.
	// [x] Seems that any memory can be registered as DMA buffers, if they satisfy the constraints:
	// 1) Corresponding physical memory is allocated. The page table is built. 
	//		If the memory is allocated by user space allocater, malloc, we need to walk  through the page table.
	// 2) The physial memory is pinned, can't be swapt/paged out.
  rdma_session->recv_buf = (struct message*)calloc(1,sizeof(struct message));  	//[?] Or do we need to allocate DMA memory by get_dma_addr ???
	rdma_session->send_buf = (struct message*)calloc(1,sizeof(struct message));  
	rdma_session->mem = DMA;   // [??] Is this useful ? Get lkey fomr rmem_session_context->dma_mr.


	// Get DMA/BUS address for the receive buffer
	rdma_session->recv_mr	=	ibv_reg_mr(rdma_session->pd, rdma_session->recv_buf, sizeof(struct message), IBV_ACCESS_LOCAL_WRITE  |
																																																				IBV_ACCESS_REMOTE_READ |
																																																				IBV_ACCESS_REMOTE_WRITE);
	if(rdma_session->recv_mr == NULL){
		ret = -1;
		printf("%s,rdma_session->recv_mr failed \n", __func__);
		goto err;
	}

	rdma_session->send_mr	=	ibv_reg_mr(rdma_session->pd, rdma_session->send_buf, sizeof(struct message), IBV_ACCESS_LOCAL_WRITE |
																																																				IBV_ACCESS_REMOTE_READ |
																																																				IBV_ACCESS_REMOTE_WRITE);
	if(rdma_session->send_mr == NULL){
		ret = -1;
		printf("%s,rdma_session->send_mr failed \n", __func__);
		goto err;
	}

	// 3) Add the allocated (DMA) buffer to reserved WRs
	octopus_setup_message_wr(rdma_session);
	printf("%s, allocated & registered buffers...\n", __func__);

	return ret;


err:

	printf( "%s, Bind DMA buffer error. \n", __func__);

	// if (cb->rdma_mr && !IS_ERR(cb->rdma_mr))
	// 	ib_dereg_mr(cb->rdma_mr);
	// if (rdma_session->dma_mr && !IS_ERR(rdma_session->dma_mr))
	if (rdma_session->dma_mr)
	 	ibv_dereg_mr(rdma_session->dma_mr);
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
		printf( "%s, rdma_connect error %d\n", __func__, ret);
		return ret;
	}else{
		printf("%s, Send RDMA connect request to remote server \n", __func__);
	}

	ret = octopus_rdma_cm_event_handler(rdma_session);
	if(ret != 0){
		printf("%s, octopus_rdma_cm_event_handler failed.\n",__func__);
		goto err;
	}

	//wait_event_interruptible(rdma_session->sem, rdma_session->state >= CONNECTED);
	sem_wait(&rdma_session->sem);
	if (rdma_session->state == ERROR) {
		printf( "%s, wait for CONNECTED state %d\n", __func__, rdma_session->state);
		return ret;
	}

	printf("%s, RDMA connect successful\n", __func__);
	return ret;

err:
	printf("Error in %s \n", __func__);
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
 *  In user space, we have to invoke ibv_get_cq_event() mannual to get the received CQ event.
 * 
 * [x] For the 1-sided RDMA read/write, there is also a WC to acknowledge the finish of this.
 */
int octopus_cq_event_handler(struct rdma_session_context 	*rdma_session){    // cq : kernel_cb->cq;  ctx : cq->context, just the kernel_cb

	bool					stop_waiting_on_cq 	= 	false;
	struct ibv_wc wc;
	struct ibv_cq *evt_cq;						// Only used for ack the sender, we received the cq event ?
	void*					cq_context_null;		// [?] What's this used for ? 
	//struct ib_recv_wr 	*bad_wr;
	int 					ret = 0;
	//BUG_ON(rdma_session->cq != cq);
	if (rdma_session->state == ERROR) {
		printf( "%s, cq completion in ERROR state\n", __func__);
		return;
	}

	// 1) Poll the CQ from CQ channel mannually.
	if (ibv_get_cq_event(rdma_session->comp_channel ,&evt_cq, &cq_context_null) != 0){
		printf("%s, ibv_get_cq_event failed.\n",__func__);
		ret = -1;
		goto err;
	}

	#ifdef DEBUG_RDMA_CLIENT
	printf("%s: Receive cq[%llu] \n", __func__, cq_get_count++);
	#endif

	// 2) ACK the received CQ
	// 	This operation is done with mutex, slow.
	ibv_ack_cq_events(evt_cq, 1);


	// Notify_cq, poll_cq are all both one-shot
	// Get notification for the next one or several wc.
	ret = ibv_req_notify_cq(rdma_session->cq, 0);   
	if (ret) {
		printf( "%s: request for cq completion notification failed \n",__func__);
		goto err;
	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printf("%s: cq_notify_count : %llu , wait for next cq_event \n",__func__, cq_notify_count++);
	}
	#endif

	// If current function, rdma_cq_event_handler, is invoked, one or several WC is on the CQE.
	// Get the SIGNAL, WC, by invoking ib_poll_cq.
	//
	//drivers/infiniband/hw/mlx4/main.c:2707:	ibdev->ib_dev.poll_cq		= mlx4_ib_poll_cq;
	//	ib_poll_cq is cq->device->poll_cq

	//if(likely( (ret = ib_poll_cq(rdma_session->cq, 1, &wc)) == 1  )) {   //ib_poll_cq, get "one" wc from cq.
	while((ret = ibv_poll_cq(rdma_session->cq, 1, &wc)) == 1  ) {
		if (wc.status != IBV_WC_SUCCESS) {   		// IB_WC_SUCCESS == 0
			// if (wc.status == IB_WC_WR_FLUSH_ERR) {
			// 	printf( "%s, cq flushed\n", __func__);
			// 	//continue;
			// 	// IB_WC_WR_FLUSH_ERR is different ??
			// 	goto err;
			// } else {
				printf( "%s, cq completion failed with wr_id 0x%llx status %d,  status name %s, opcode %d,\n",__func__,
																wc.wr_id, wc.status, rdma_wc_status_name(wc.status), wc.opcode);
				goto err;
			//}
		}	

		switch (wc.opcode){
			case IBV_WC_RECV:				
				#ifdef DEBUG_RDMA_CLIENT
				printf("%s, Got a WC from CQ, IB_WC_RECV. \n", __func__);
				#endif
				// Need to do actions based on the received message type.
				ret = handle_recv_wr(rdma_session, &wc);
			  	if (ret) {
				 	printf( "%s, recv wc error: %d\n", __func__, ret);
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
				 //	printf( PFX "post recv error: %d\n",    ret);
				 //	goto error;
				 //}
				// if (cb->state == RDMA_BUF_ADV || cb->state == FREE_MEM_RECV || cb->state == WAIT_OPS){
				// 	wake_up_interruptible(&cb->sem);
				// }

				break;
			case IBV_WC_SEND:

				// nothing need to be done for a IB_WC_SEND.

				// ret = client_send(cb, &wc);
				// if (ret) {
				// 	printf( PFX "send wc error: %d\n", ret);
				// 	goto error;
				// }

				#ifdef DEBUG_RDMA_CLIENT
				printf("%s, Got a WC from CQ, IB_WC_SEND, then wait for receive RDMA mesage.. \n", __func__);
				#endif

				 break;
			case IBV_WC_RDMA_READ:
				 
				#ifdef DEBUG_RDMA_CLIENT
				printf("%s, Got a WC from CQ, IB_WC_RDMA_READ \n", __func__);
				#endif
				 
				 ret = rdma_read_done( &wc);
				 if (ret) {
				 	printf( "%s, Handle cq event, IB_WC_RDMA_READ, error \n", __func__);
				 	goto err;
				 }
				break;
			case IBV_WC_RDMA_WRITE:
				ret = rdma_write_done(&wc);
				 if (ret) {
				 	printf( "%s, Handle cq event, IB_WC_RDMA_WRITE, error \n", __func__);
				 	goto err;
				 }

				#ifdef DEBUG_RDMA_CLIENT
				printf("%s, Got a WC from CQ, IB_WC_RDMA_WRITE \n", __func__);
				#endif

				break;
			default:
				printf( "%s:%d Unexpected opcode %d, Shutting down\n", __func__, __LINE__, wc.opcode);
				goto err;
		} // switch

		//
		// Notify_cq, poll_cq are all both one-shot
		// Get notification for the next wc.
		// ret = ib_req_notify_cq(rdma_session->cq, IB_CQ_NEXT_COMP);   
		// if (unlikely(ret)) {
		// 	printf( "%s: request for cq completion notification failed \n",__func__);
		// 	goto err;
		// }
		// #ifdef DEBUG_RDMA_CLIENT
		// else{
		// 	printf("%s: cq_notify_count : %llu , wait for next cq_event \n",__func__, cq_notify_count++);
		// }
		// #endif

	} // poll 1 cq   in a loop.
	// else{
	// 	printf( "%s, poll error %d\n", __func__, ret);
	// 	goto err;
	// }

	return ret;
err:
	printf( "ERROR in %s \n",__func__);
	rdma_session->state = ERROR;
	octopus_disconenct_and_collect_resource(rdma_session);  // Disconnect and free all the resource.
	return ret;
}




/**
 * Send a RDMA message to remote server.
 * 
 * 
 */
int send_message_to_remote(struct rdma_session_context *rdma_session, int messge_type  , int size_gb){
	int ret = 0;
	struct ibv_send_wr * bad_wr;
	rdma_session->send_buf->type = messge_type;
	rdma_session->send_buf->size_gb = size_gb; 		// chunk_num = size_gb/chunk_size

	#ifdef DEBUG_RDMA_CLIENT
	printf("Send a Message to Remote memory server. cb->send_buf->type : %d, %s \n", messge_type, rdma_message_print(messge_type) );
	#endif

	ret = ibv_post_send(rdma_session->qp, &rdma_session->sq_wr, &bad_wr);
	if (ret) {
		printf( "%s: BIND_SINGLE MSG send error %d\n", __func__, ret);
		return ret;
	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printf("%s: 2-sided RDMA message[%llu] send. \n",__func__,rmda_ops_count++);
	}
	#endif

	// handle the post_send signal.
	ret = octopus_cq_event_handler(rdma_session);
	if(ret){
		printf("%s, octopus_cq_event_handler : SEND_DONE failed. \n",__func__);
		goto err;
	}

	return ret;	

err:
	printf("ERROR in %s \n",__func__);
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
int handle_recv_wr(struct rdma_session_context *rdma_session, struct ibv_wc *wc){
	int ret;
	int i;		// Some local index

	if ( wc->byte_len != sizeof(struct message) ) {         // Check the length of received message
		printf( "%s, Received bogus data, size %d. Expected Message size 0x%lx \n", 
																																			__func__,  
																																			wc->byte_len,
																																			(unsigned long)sizeof(struct message));
		return -1;
	}	

	#ifdef DEBUG_RDMA_CLIENT
	// Is this check necessary ??
	if (rdma_session->state < CONNECTED ) {
		printf( "%s, RDMA is not connected\n", __func__);	
		return -1;
	}
	#endif


	#ifdef DEBUG_RDMA_CLIENT
	printf("%s, Recieved RDMA message: %s \n",__func__, rdma_message_print(rdma_session->recv_buf->type));
	#endif

	switch(rdma_session->recv_buf->type){
		case FREE_SIZE:
			//
			// Step 1), get the Free Regions. 
			//
			#ifdef DEBUG_RDMA_CLIENT
			printf( "%s, avaible size : %d GB \n ", __func__,	rdma_session->recv_buf->size_gb );
			#endif

			rdma_session->remote_chunk_list.remote_free_size_gb = rdma_session->recv_buf->size_gb;  // Received free memory size
			rdma_session->state = FREE_MEM_RECV;	
			
			// Only receive the total free size of remote memory pool
			// 1st part, a meta data Region
			// 2nd part, data data Regions
			ret = init_remote_chunk_list(rdma_session);  
			if(ret){
				printf( "Initialize the remote chunk failed. \n");
			}

			// Step 1) finished.
			//wake_up_interruptible(&rdma_session->sem);
			sem_post(&rdma_session->sem);

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
			//wake_up_interruptible(&rdma_session->sem);  // Finish main function.
			sem_post(&rdma_session->sem);

			break;
		case GOT_SINGLE_CHUNK:
		//	cb->IS_sess->cb_state_list[cb->cb_index] = CB_MAPPED;
			//rdma_session->state = WAIT_OPS;
		

			#ifdef DEBUG_RDMA_CLIENT 
			// Check the received data
			// All the rkey[] are reset to 0 before sending to client.
			for(i=0; i< MAX_REGION_NUM; i++){
				if(rdma_session->recv_buf->rkey[i]){
					printf("%s, received remote chunk[%d] addr : 0x%llx, rkey : 0x%x \n", __func__, i,
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
			printf( "%s, Recieved RDMA message UN-KNOWN \n",__func__);
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
	struct ibv_recv_wr *bad_wr;

	if(num_chunk == 0 || rdma_session == NULL)
		goto err;

	ret = ibv_post_recv(rdma_session->qp, &rdma_session->rq_wr, &bad_wr);
	if(ret) {
		printf( "%s, Post 2-sided message to receive data failed.\n", __func__);
		goto err;
	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printf("%s: 2-sided RDMA message[%llu] recv. \n",__func__,rmda_ops_count++);
	}
	#endif

	// Post the send WR
	ret = send_message_to_remote(rdma_session, REQUEST_CHUNKS, num_chunk * REGION_SIZE_GB );
	if(ret) {
		printf( "%s, Post 2-sided message to remote server failed.\n", __func__);
		goto err;
	}

	// Handle the post_recv signal.
	ret	=	octopus_cq_event_handler(rdma_session);
	if(ret){
		printf("%s, octopus_cq_event_handler : RECEIVED_CHUNKS failed. \n",__func__);
		goto err;
	}


	return ret;

	err:
	printf( "Error in %s \n", __func__);
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
 * Allocate and intialize enough WR and DMA buffers used for 1-sided RDMA messages.
 * 
 * [?] For this design, when transffer i/o request to RDMA message, one more cpu copy is needed:
 * 		Store  : need to copy all the data from physical page to DMA buffer. async, not latency sen
 * 		Load, read/write page fault : Copy data from DMA buffer to physical pages.
 * 
*/
int init_rdma_command_list(struct rdma_session_context *rdma_session, uint32_t queue_ind){
	int ret = 0;
	uint32_t i;
	struct rmem_rdma_queue		*rdma_q_ptr;
	struct rmem_rdma_command 	*rdma_cmd_ptr;  // rmem_rdma_command iterator.


	rdma_q_ptr = &(rdma_session->rmem_rdma_queue_list[queue_ind]);
	rdma_q_ptr->cmd_ptr	= 0;  // Start from 0.
	rdma_q_ptr->rdma_queue_depth	= RMEM_QUEUE_DEPTH;	// This value should match "rmem_dev_ctrl->queue_depth" 
	rdma_q_ptr->q_index	= 0;				// The corresponding dispatch queue index, assigned in blk_mq_ops->.init_hctx.
	rdma_q_ptr->rdma_session = rdma_session;
	// Build the rdma_command list.
	rdma_q_ptr->rmem_rdma_cmd_list = (struct rmem_rdma_command*)calloc(1, sizeof(struct rmem_rdma_command) * rdma_q_ptr->rdma_queue_depth );

	//initialize the spinlock
	pthread_spin_init(&(rdma_q_ptr->rdma_queue_lock), PTHREAD_PROCESS_SHARED);

	// Initialize each rmem_rdma_command
	for(i=0; i< rdma_q_ptr->rdma_queue_depth; i++){
		rdma_cmd_ptr = &(rdma_q_ptr->rmem_rdma_cmd_list[i]);

		// Set its flag
		//atomic_set(&(rdma_cmd_ptr->free_state), 1);
		rdma_cmd_ptr->free_state = 1;

		// [x] the I/O request can contain mulplte bios, each bio can have multiple segments.
		// Can't dertermine the good number for the rdma_buf size now. 
		// 64 pages per rdma_buf are enough now.
		rdma_cmd_ptr->rdma_buf = (char*)calloc(1, ONE_SIEDED_RDMA_BUF_SIZE); 
		// rdma_cmd_ptr->rdma_dma_addr = ib_dma_map_single(rdma_session->pd->device,
		// 																	rdma_q_ptr->rmem_rdma_cmd_list[i].rdma_buf,
		// 																	ONE_SIEDED_RDMA_BUF_SIZE,
		// 																	DMA_BIDIRECTIONAL);
		// [x]Latter, we can change to use the MACRO for space efficiency
		// pci_unmap_addr_set(ctx, rdma_mapping, rdma_q_ptr->rmem_rdma_cmd_list[i]->rdma_dma_addr);
		rdma_cmd_ptr->rdma_mr	= ibv_reg_mr(rdma_session->pd, rdma_cmd_ptr->rdma_buf, ONE_SIEDED_RDMA_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE |
																																																						IBV_ACCESS_REMOTE_WRITE |
																																																						IBV_ACCESS_REMOTE_READ);
		if(rdma_cmd_ptr->rdma_mr == NULL){
			ret = -1;
			printf("%s, ibv_reg_mr failed. \n",__func__);
			goto err;
		}

		rdma_cmd_ptr->rdma_sgl.addr 	= (uintptr_t)rdma_cmd_ptr->rdma_buf;
		rdma_cmd_ptr->rdma_sgl.length	=	ONE_SIEDED_RDMA_BUF_SIZE;
		rdma_cmd_ptr->rdma_sgl.lkey		= rdma_cmd_ptr->rdma_mr->lkey;


		rdma_cmd_ptr->rdma_sq_wr.opcode	=	IBV_WR_SEND;
		rdma_cmd_ptr->rdma_sq_wr.send_flags = IBV_SEND_SIGNALED; // 1-sided RDMA message ? both read /write
		rdma_cmd_ptr->rdma_sq_wr.sg_list = &(rdma_cmd_ptr->rdma_sgl);
		
		// An I/O request can have multiple bio, and each bio can have multiple segments, which is logial sector ?
		// But we can copy the segments from i/o request to DMA buffer and compact them together. 
		rdma_cmd_ptr->rdma_sq_wr.num_sge	= 1; 

		// ib_rdma_wr->ib_send_wr->wr_id is the driver_data.
		// Seem that wc->wr_id is gotten from ib_send_wr.wr_id.
		rdma_cmd_ptr->rdma_sq_wr.wr_id	= (uint64_t)rdma_cmd_ptr; 

		// queue_rq use this field to connect to a request
		//rdma_cmd_ptr->io_rq = NULL;
	}

	return ret;

err:
	printf( "ERROR in %s \n",__func__);
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
	// 	printf("%s, rmem_dev_ctrl is NULL. ERROR! \n",__func__);
	// #endif

	// One rdma_command_list per dispatch queue.
	// One rdma_command per I/O request.
	rdma_session->rmem_rdma_queue_list = (struct rmem_rdma_queue *)calloc(1, sizeof(struct rmem_rdma_queue)*rdma_q_num );


	for(i=0; i<rdma_q_num; i++  ){
		init_rdma_command_list(rdma_session, i);  // Initialize the ith rdma_queue.
	}


	#ifdef DEBUG_RDMA_CLIENT
	printf("%s, Intialize rdma_session_context->rmem_rdma_queue_list done. \n",__func__);
	#endif


	return ret;

err:
	printf("ERROR in %s \n",__func__);
	return ret;
}


/**
 * Gather the I/O request data and build the 1-sided RDMA write
 *  Invoked by Block Deivce part. Don't declare it as static.
 * 
 * 
 */
int post_rdma_write(struct rdma_session_context *rdma_session, struct request* io_rq, struct rmem_rdma_queue* rdma_q_ptr,  
					struct remote_mapping_chunk *remote_chunk_ptr, uint64_t offse_within_chunk, uint64_t len ){

	int ret = 0;
	//int cpu;
	

	return ret;

err:
	printf( "ERROR in %s \n",__func__);
	return ret;
}



/**
 * 1-sided RDMA write is done.
 * 		Write data into remote memory pool successfully.
 * 		
 */
int rdma_write_done(struct ibv_wc *wc){


	return 0;
}





/**
 * Copy data from I/O request to RDMA buffer.
 * 
 * [?] Can we aviod this data copy ??
 * 
 * [?] How many pages can be attached to each request??
 * 		Each request can have multiple bio,
 * 		Each bio can have multiple segments. 
 * 
 * [?] The size of rdma_ptr->rdma_buf is only 4K !!!!
 * 
 */
void copy_data_to_rdma_buf(struct request *io_rq, struct rmem_rdma_command *rdma_ptr){



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
	

	return ret;

err:
	printf( " ERROR in %s. \n",__func__);
	return ret;
}



/**
 * 1-sided RDMA read is done.
 * Read data back from the remote memory server.
 * Put data back to I/O request and send it back to upper layer.
*/
int rdma_read_done(struct ibv_wc *wc){
	int ret = 0;
  

err:
	return ret;
}



//
// [?]Do we need  to consider the concurent problems ??
//		Every dispatch queue can only use its own rdma_queue.
//
//	Here may cause override problems caused by concurrency problems. !!!!! get the same rdma_cmd_ind ??
//
//		https://lwn.net/Articles/695257/
struct rmem_rdma_command* get_a_free_rdma_cmd_from_rdma_q(struct rmem_rdma_queue* rmda_q_ptr){

	int rdma_cmd_ind  = 0;
	unsigned long flags;
	//int cpu;

	// Fast path, use the slots pointed by rmem_rdma_queue->ptr.

	// 					TO BE DONE.

	//
	// Slow path,  traverse the list to find one.
	// Adjust the pointer.
	// But must make sure that the slots behind the pointer, rmem_rdma_queue->ptr, are all available.
	//while(rdma_cmd_ind != rmda_q_ptr->rdma_queue_depth - 1){
	
	//cpu = get_cpu();

	// critical section
	//spin_lock(&(rmda_q_ptr->rdma_queue_lock));

	//debug
	// Spin lock invocation cause error ?
	// spin lock disable all the interrup,  printf cause irq ???
	//spin_lock_irqsave(&(rmda_q_ptr->rdma_queue_lock), flags);
	//printf("%s, hehe \n",__func__); // printf will cause interrupt ? Will this cause error ? no. This will not cause error.
	//atomic_read(&(rmda_q_ptr->rmem_rdma_cmd_list[rdma_cmd_ind].free_state));  // Can NOT use this atomic_* operation within spin_lock_*
	//spin_unlock_irqrestore(&(rmda_q_ptr->rdma_queue_lock), flags);

	while(1){

		//debug
		printf("%s: TRY to get rdma_queue[%d] rdma_cmd[%d].\n",__func__, rmda_q_ptr->q_index, rdma_cmd_ind);

		//busy waiting.
		pthread_spin_lock(&(rmda_q_ptr->rdma_queue_lock));
		// if( atomic_read(&(rmda_q_ptr->rmem_rdma_cmd_list[rdma_cmd_ind].free_state)) == 1 ){  // 1 means available.
		// 	atomic_set(&(rmda_q_ptr->rmem_rdma_cmd_list[rdma_cmd_ind].free_state), 0);  // [??] This can't solve the concurry prolbems.
		// 	//debug
		// 	printf("%s: SUCC rdma_queue[%d]  get rdma_cmd[%d] \n",__func__,rmda_q_ptr->q_index ,rdma_cmd_ind);
			
		// 	return &(rmda_q_ptr->rmem_rdma_cmd_list[rdma_cmd_ind]);  // move forward
		// }


		if( rmda_q_ptr->rmem_rdma_cmd_list[rdma_cmd_ind].free_state == 1 ){  // 1 means available.
			rmda_q_ptr->rmem_rdma_cmd_list[rdma_cmd_ind].free_state =0;  // [??] This can't solve the concurry prolbems.
			
			//debug
			pthread_spin_unlock(&(rmda_q_ptr->rdma_queue_lock));
			printf("%s: SUCC rdma_queue[%d]  get rdma_cmd[%d] \n",__func__,rmda_q_ptr->q_index ,rdma_cmd_ind);
			return &(rmda_q_ptr->rmem_rdma_cmd_list[rdma_cmd_ind]);  // move forward
		}

		if(rdma_cmd_ind++ == rmda_q_ptr->rdma_queue_depth - 1){
			rdma_cmd_ind = 0;  
		}

		// unlock
		pthread_spin_unlock(&(rmda_q_ptr->rdma_queue_lock));

	}// while.
	//put_cpu();

	//spin_unlock_irqrestore(&(rmda_q_ptr->rdma_queue_lock), flags);
	//spin_unlock(&(rmda_q_ptr->rdma_queue_lock));

//err:
	printf( "%s, Current rmem_rdma_queue[%d] : runs out rmem_rdma_cmd \n",__func__, rmda_q_ptr->q_index);
	return NULL;
}

// Set this rdma_command free
// Free is a concurrent safe procedure
inline void free_a_rdma_cmd_to_rdma_q(struct rmem_rdma_command* rdma_cmd_ptr){
	//atomic_set(&(rdma_cmd_ptr->free_state), 1);
	rdma_cmd_ptr->free_state = 1;
//	rdma_cmd_ptr->io_rq 	=	NULL;
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
	uint32_t received_chunk_num; 
	
	// 1) initialize chunk related variables
	//		The first Region may not be fullly mapped.
	//		The 2nd -> rest are fully mapped at REGION_SIZE_GB size.
	if(rdma_session->remote_chunk_list.remote_free_size_gb % REGION_SIZE_GB != 0){
		received_chunk_num = rdma_session->remote_chunk_list.remote_free_size_gb/REGION_SIZE_GB + 1;
	}else{
		received_chunk_num = rdma_session->remote_chunk_list.remote_free_size_gb/REGION_SIZE_GB;
	}
	rdma_session->remote_chunk_list.chunk_ptr = 0;	// Points to the first empty chunk.


	rdma_session->remote_chunk_list.chunk_num = received_chunk_num;
	rdma_session->remote_chunk_list.remote_chunk = (struct remote_mapping_chunk*)calloc(1,sizeof(struct remote_mapping_chunk) * received_chunk_num);

	for(i=0; i < received_chunk_num; i++){
		rdma_session->remote_chunk_list.remote_chunk[i].chunk_state = EMPTY;
		rdma_session->remote_chunk_list.remote_chunk[i].remote_addr = 0x0;
		rdma_session->remote_chunk_list.remote_chunk[i].mapped_size	= 0x0;
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
	for(i = 0; i < MAX_REGION_NUM; i++ ){
		
		#ifdef DEBUG_RDMA_CLIENT
			if( *chunk_ptr >= rdma_session->remote_chunk_list.chunk_num){
				printf( "%s, Get too many chunks. \n", __func__);
				break;
			}
		#endif
		
		// If the rkey[] is NOT null, means we recieved data.
		if(rdma_session->recv_buf->rkey[i]){
			// Sent chunk, attach to current chunk_list's tail.
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_rkey = rdma_session->recv_buf->rkey[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_addr = rdma_session->recv_buf->buf[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].mapped_size = rdma_session->recv_buf->mapped_size[i];
			rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].chunk_state = MAPPED;
			

			#ifdef DEBUG_RDMA_CLIENT
				printf("Got chunk[%d] : remote_addr : 0x%llx, remote_rkey: 0x%x, mapped_size: 0x%llx \n", i, 
																						rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_addr,
																						rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].remote_rkey,
																						rdma_session->remote_chunk_list.remote_chunk[*chunk_ptr].mapped_size);
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
	char* 	ip_str = "10.0.0.2";
	uint16_t	port = 9400;
	struct ibv_recv_wr *bad_wr;

	//
 	// 1) init rdma_session_context
	// 		rdma_session points a the global vatiable , rdma_seesion_global.
	//		Initialize its fileds directly.

	rdma_session->event_channel	=	rdma_create_event_channel();
	if(rdma_session->event_channel == NULL){
		ret = -1;
		goto err;
	}
	
	//(struct rdma_event_channel *channel, struct rdma_cm_id **id, void *context, enum rdma_port_space ps)
	//   [?]context is assigned to rdma_cm_id->verbs (ibv_verbs)  or rdma_cm_id->context ?
	//					Should be rdma_create_id->context. self defined context.
	//					rdma_cm_id->verbs is used for ibv_* functions.
	//
	ret =	rdma_create_id( rdma_session->event_channel, &(rdma_session->cm_id), (void*)rdma_session,  RDMA_PS_TCP);
	if(ret)
		goto err;




	// Used for a async   
	rdma_session->state = IDLE;

  	// The number of  on-the-fly wr ?? every cores handle one ?? 
	// This depth is used for CQ entry ? send/receive queue depth ?
  rdma_session->txdepth = RDMA_READ_WRITE_QUEUE_DEPTH * online_cores + 1; //[?] What's this used for ? What's meaning of the value ?
	rdma_session->freed = 0;	// Flag of functions' called number. 

  // Setup socket information
	// Debug : for test, write the ip:port as 10.0.0.2:9400
  rdma_session->port = htons(port);  // After transffer to big endian, the decimal value is 47140
  ret= inet_pton(AF_INET, ip_str, rdma_session->addr);   // char* to ipv4 address sequence. 
  if(ret == 0){  		// kernel 4.11.0 , success 1; failed 0.
		printf("Assign ip %s to  rdma_session->addr : %s failed.\n",ip_str, rdma_session->addr );
	}
	rdma_session->addr_type = AF_INET;  //ipv4


  	// Initialize the queue.
  //init_waitqueue_head(&rdma_session->sem);
	//
	// User space semaphore:
	// 2nd parameter, 0, means shared between threads.
	// 3rd parameter, initial value of the semaphore 
	//		sem_wai() decrease semaphore value, if the value is not positive, then thread wait on semaphore.
	//		sem_post() increase semaphore value and wake up a thread blocked on it.
	ret = sem_init(&rdma_session->sem, 0, 1); 
	if(ret != 0){
		printf("%s, sem_init failed. \n");
		goto err;
	}
	#ifdef DEBUG_RDMA_CLIENT
  	printf("%s, Initialized rdma_session_context fields successfully.\n", __func__);
	#endif

  //2) Resolve address(ip:port) and route to destination IB. 
  ret = rdma_resolve_ip_to_ib_device(rdma_session);
	if (ret){
		printf ( "%s, bind socket error (addr or route resolve error)\n", __func__);
    return ret;
  }
	#ifdef DEBUG_RDMA_CLIENT
	else{
    printf("%s,Binded to remote server successfully.\n", __func__);
  }
	#endif

  // 3) Create the QP,CQ, PD
	//  Before we connect to remote memory server, we have to setup the rdma queues, CQ, QP.
	//	We also need to register the DMA buffer for two-sided communication and configure the Protect Domain, PD.
	//
	// Build the rdma queues.
  ret = octopus_create_rdma_queues(rdma_session, rdma_session->cm_id);
  if(ret){
		printf( "%s, Create rdma queues failed. \n", __func__);
  }

	// 4) Register some message passing used DMA buffer.
	// 

	// 4.1) 2-sided RDMA message intialization
	//rdma_session->mem = DMA;
	// !! Do nothing for now.
  ret = octopus_setup_buffers(rdma_session);
	if(ret){
		printf( "%s, Bind DMA buffer error\n", __func__);
	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printf("%s, Allocate and Bind DMA buffer successfully \n", __func__);
	}
	#endif

	// 4.2) 1-sided RDMA message intialization
	//
	ret = init_rdma_command_queue_list(rdma_session);
	if(ret){
		printf( "%s, initialize 1-sided RDMA buffers error. \n",__func__);
		goto err;
	}



	// 5) Build the connection to Remote

	// After connection, send a QUERY to query and map  the available Regions in Memory server. 
	// Post a recv wr to wait for the FREE_SIZE RDMA message, sent from remote memory server.
	//
	ret = ibv_post_recv(rdma_session->qp, &rdma_session->rq_wr, &bad_wr); 
	if(ret){
		printf( "%s: post a 2-sided RDMA message error \n",__func__);
		goto err;
	}	
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printf("%s: 2-sided RDMA message[%llu] send. \n",__func__, rmda_ops_count++);
	}
	#endif

	// Build RDMA connection.
	ret = octopus_connect_remote_memory_server(rdma_session);
	if(ret){
		printf( "%s: Connect to remote server error \n", __func__);
		goto err;
	}
	#ifdef DEBUG_RDMA_CLIENT
	else{
		printf("%s, CPU server state should be CONNECTED, actual is  : %s \n", __func__,
																							rdma_session_context_state_print(rdma_session->state) );
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
	ret = ibv_req_notify_cq(rdma_session->cq, 0);   
	if (ret) {
		printf( "%s, ib_create_cq failed\n", __func__);
		goto err;
	}
	#ifdef DEBUG_RDMA_CLIENT 
	else{
		printf("%s: cq_notify_count : %llu \n",__func__, cq_notify_count++);
	}
	#endif


	ret	=	octopus_cq_event_handler(rdma_session);
	if(ret){
		printf("%s, octopus_cq_event_handler : FREE_MEM_RECV failed. \n",__func__);
		goto err;
	}

	// Sequence controll
	//wait_event_interruptible( rdma_session->sem, rdma_session->state == FREE_MEM_RECV );
	sem_wait(&rdma_session->sem);
	printf("%s,CPU server state should be FREE_MEM_RECV, actual is : %s  \n", 
																																																						__func__,
																																		rdma_session_context_state_print(rdma_session->state) );

	// b. Post the receive WR.
	// Should post this recv WR before connect the RDMA connection ?
	//struct ib_recv_wr *bad_wr;
	//ret = ib_post_recv(rdma_session->qp, &rdma_session->rq_wr, &bad_wr); 


	//7) Post a message to Remote memory server.
	//wait_event_interruptible(rdma_session->sem, rdma_session->state == SEND_MESSAGE);
	//printf("Receive message down, wake up to send message.\n");
	//send_messaget_to_remote(rdma_session, 9, 2);  // 9 : bind_single
	//send_messaget_to_remote(cb, 6, 2);  // 6 : activity


	// Send a RDMA message to request for mapping a chunk ?
	// Parameters
	// 		rdma_session_context : driver data
	//		number of requeted chunks
	#ifdef DEBUG_RDMA_CLIENT
	printf("%s: Request for Chunks bind: %u \n",__func__, MAX_REGION_NUM);
	#endif
	ret = octupos_requset_for_chunk(rdma_session, MAX_REGION_NUM);  // 8 chunk, 1 GB/chunk.
	if(ret){
		printf("%s, Bind chunks failed.\n", __func__);
		goto err;
	}


	// Sequence controll
	//wait_event_interruptible( rdma_session->sem, rdma_session->state == RECEIVED_CHUNKS );
	sem_wait(&(rdma_session->sem));
	printf("%s,CPU server state should be RECEIVED_CHUNKS, actual is : %s  \n", 
																																																						__func__,
																																		rdma_session_context_state_print(rdma_session->state) );


	// SECTION 2
	// [!!] Connect to Disk Driver  [!!]
	//
	// rdma_session->rmem_dev_ctrl = &rmem_dev_ctrl_global;
	// rmem_dev_ctrl_global.rdma_session = rdma_session;		// [!!]Do this before intialize Block Device.
	
	// #ifndef DEBUG_RDMA_ONLY
	// ret =rmem_init_disk_driver(&rmem_dev_ctrl_global);
	// #endif

	// if(unlikely(ret)){
	// 	printf("%s, Initialize disk driver failed.\n", __func__);
	// 	goto err;
	// }



	// SECTION 3, FINISHED.

	// [!!] Only reach here afeter got STOP_ACK signal from remote memory server.
	// Sequence controll - FINISH.
	// Be carefull, this will lead to all the local variables collected.

	// [?] Can we wait here with function : octopus_disconenct_and_collect_resource together?
	//  NO ! https://stackoverflow.com/questions/16163932/multiple-threads-can-wait-on-a-semaphore-at-same-time
	// Use  differeent semapore signal.
	//wait_event_interruptible(rdma_session->sem, rdma_session->state == CM_DISCONNECT);

	//wait_event_interruptible( rdma_session->sem, rdma_session->state == TEST_DONE );



	printf("%s,Exit the main() function.\n", __func__);

	return ret;

err:
	//free resource
	
	// Free the rdma_session at last. 
	// if(rdma_session != NULL)
	// 	free(rdma_session);

	printf( "ERROR in %s \n", __func__);
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

	uint32_t i,j;
	uint32_t rdma_q_num = online_cores;
	struct rmem_rdma_queue		*rdma_q_ptr;
	struct rmem_rdma_command	*rdma_cmd_ptr;

	// Free the DMA buffer for 2-sided RDMA messages
	if(rdma_session == NULL)
		return;

	// Free 1-sided dma buffers
	if(rdma_session->recv_buf != NULL)
		free(rdma_session->recv_buf);
	if(rdma_session->send_buf != NULL)
		free(rdma_session->send_buf);

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
					if(rdma_cmd_ptr->rdma_buf != NULL){
						free(rdma_cmd_ptr->rdma_buf);

						#ifdef DEBUG_RDMA_CLIENT_DETAIL
						printf("%s: free rdma_queue[%d], rdma_cmd_buf[%d]", __func__,i,j );
						#endif
					}

				} // for - j

				free(rdma_q_ptr->rmem_rdma_cmd_list);
			}
			
		}// for - i

		// free rdma queue
		free(rdma_session->rmem_rdma_queue_list);
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
		free(rdma_session->remote_chunk_list.remote_chunk);

	#ifdef DEBUG_RDMA_CLIENT
	printf("%s, Free RDMA buffers done. \n",__func__);
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
		printf("%s, free rdma_cm_id done. \n",__func__);
		#endif
	}

	if(rdma_session->qp != NULL){
		ibv_destroy_qp(rdma_session->qp);
		//rdma_destroy_qp(rdma_session->cm_id);

		#ifdef DEBUG_RDMA_CLIENT
		printf("%s, free ib_qp  done. \n",__func__);
		#endif
	}

	// 
	// Both send_cq/recb_cq should be freed in ib_destroy_qp() ?
	//
	if(rdma_session->cq != NULL){
		ibv_destroy_cq(rdma_session->cq);

		#ifdef DEBUG_RDMA_CLIENT
		printf("%s, free ibv_cq  done. \n",__func__);
		#endif
	}

	// Before invoke this function, free all the resource binded to pd.
	if(rdma_session->pd != NULL){
		ibv_dealloc_pd(rdma_session->pd); 

		#ifdef DEBUG_RDMA_CLIENT
		printf("%s, free ib_pd  done. \n",__func__);
		#endif
	}

	#ifdef DEBUG_RDMA_CLIENT
	printf("%s, Free RDMA structures,cm_id,qp,cq,pd done. \n",__func__);
	#endif

}


/**
 * The main entry of resource free.
 * 
 * [x] 2 call site.
 * 		1) Called by client, at the end of function, octopus_rdma_client_cleanup_module().
 * 		2) Triggered by DISCONNECT CM event, in octopus_rdma_cm_event_handler().
 * 
 */
int octopus_disconenct_and_collect_resource(struct rdma_session_context *rdma_session){

	int ret = 0;

	if(rdma_session->freed != 0){
		// already called by some thread,
		// just return and wait.
		return 0;
	}
	rdma_session->freed++;

	// The RDMA connection maybe already disconnected.
	if(rdma_session->state != CM_DISCONNECT){
		ret = rdma_disconnect(rdma_session->cm_id);
		if(ret){
			printf( "%s, RDMA disconnect failed. \n",__func__);
		}

		// wait the ack of RDMA disconnected successfully
		//wait_event_interruptible(rdma_session->sem, rdma_session->state == CM_DISCONNECT); 
		sem_wait(&(rdma_session->sem));
	}


	#ifdef DEBUG_RDMA_CLIENT
	printf("%s, RDMA disconnected, start to free resoutce. \n", __func__);
	#endif

	// Free resouces
	octopus_free_buffers(rdma_session);
	octopus_free_rdma_structure(rdma_session);

	// If not allocated by kzalloc, no need to free it.
	//free(rdma_session);  // Free the RDMA context.
	

	// DEBUG -- 
	// Exit the main function first.
	// wake_up_interruptible will cause context switch, 
	// Just skip the code below this invocation.
	//rdma_session_global.state = TEST_DONE;
	//wake_up_interruptible(&(rdma_session_global.sem));


	#ifdef DEBUG_RDMA_CLIENT
	printf("%s, RDMA memory resouce freed. \n", __func__);
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
int  octopus_rdma_client_init_module(void){

	int ret = 0;
	//printf("Do nothing for now. \n");
	printf("%s, octopus - kernel level rdma client.. \n", __func__);


	//main(0,NULL);

	//	online_cores = num_online_cpus();
	online_cores	=	16;

	ret = octopus_RDMA_connect(&rdma_session_global);
	if(ret){
		printf( "%s, octopus_RDMA_connect failed. \n", __func__);
	}

	return ret;
}


// invoked by rmmod 
void  octopus_rdma_client_cleanup_module(void){
  	
	int ret;
	
	printf(" Prepare to disconnect RDMA and free all the resource .\n");

	// 
	//[?] Should send a disconnect event to remote memory server?
	//		Invoke free_qp directly will cause kernel crashed. 
	//

	//octopus_free_qp(rdma_session_global);
	//IS_free_buffers(cb);  //!! DO Not invoke this.
	//if(cb != NULL && cb->cm_id != NULL){
	//	rdma_disconnect(cb->cm_id);
	//}

	ret = octopus_disconenct_and_collect_resource(&rdma_session_global);
	if(ret){
		printf( "%s, octopus_disconenct_and_collect_resource  failed.\n",  __func__);
	}

	//
	// 2)  Free the Block Device resource
	//
	// [!!] This cause kernel crashes, not know the reason now.
	//


	// 3) Kill all the running pthreads.
	printf("%s, Exit all the running pthreads. \n",__func__);
	pthread_exit(NULL);


	return;
}

//module_init(octopus_rdma_client_init_module);
//module_exit(octopus_rdma_client_cleanup_module);



/**
 * Main function.
 * 
 */

int main(int argc, char* argv[]){

	// Start build the RDMA 
	octopus_rdma_client_init_module();

	// free resource
	octopus_rdma_client_cleanup_module();


	return 0;
}





/**
 * >>>>>>>>>>>>>>> Start of Debug functions >>>>>>>>>>>>>>>
 *
 */


//
// Print the RDMA Communication Message 
//
char* rdma_cm_message_print(int cm_message_id){
	char* message_type_name = (char*)calloc(1, 32); // 32 bytes

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
	char* message_type_name = (char*)calloc(1, 32); // 32 bytes

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
	message_type_name = (char*)calloc(1, 32); // 32 bytes

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
	rdma_seesion_state_name = (char*)calloc(1, 32); // 32 bytes.

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


