#include "rdma_comm.hpp"



//
// Define global variables
struct context *global_rdma_ctx = NULL;					// The RDMA controll context.
//struct rdma_mem_pool* global_mem_pool = NULL;

//
// >>>>>>>>>>>>>>>>>>>>>>  Start of RDMA connection initialization >>>>>>>>>>>>>>>>>>>>>>
//


/**
 * The main entry.
 * 1) Build RDMA connection to the CPU server.
 * 	1.1) Bind information to IB hardware.  i.e. cm_id,  event_channel, listen, ip:port .
 * 	1.2) Build self-defined IB context and assignt it to rdma_cm_id->context.
 * 			i.e. communication_channel, QP, CQ, Send/Recv buffers.
 * 
 * 2) Pass down the heap information to RDMA module and register them as RDMA buffer.
 * 
 * 3) Create a daemon thread to run this Main function, handle the CM evetn.
 * 		Then this thread will create another one daemon thread, poll_cq to handle the RDMA evetn.
 * 
 * 
 * Parameters
 *		heap_start	: start address of Java heap.
 *		heap_size		: The byte size to be registered as RDMA buffer.
 */
void* Build_rdma_to_cpu_server(void* _args ){

  struct sockaddr_in6 	addr;
  struct rdma_cm_event 	*event = NULL;
  struct rdma_cm_id 		*listener = NULL;
  struct rdma_event_channel *ec = NULL;   // [?] Event channel ?
  uint16_t 	port = 0;
	char* heap_start	= NULL;
	size_t heap_size	=	0;
  const char* port_str  = "9400";
  const char* ip_str    = "10.0.0.2";

	// Parse the paramters
	struct rdma_main_thread_args *args = (struct rdma_main_thread_args *) _args;
	heap_start	=	(char*)args->heap_start;
	heap_size		=	(size_t)args->heap_size;

	#ifdef DEBUG_RDMA_SERVER
	tty->print("%s, Register Semeru Space: 0x%llx, size : 0x%llx \n",__func__, 
                                                    (unsigned long long)heap_start, 
                                                    (unsigned long long)heap_size);
	#endif


  memset(&addr, 0, sizeof(addr));  // not struct sockaddr_in6
  addr.sin6_family = AF_INET6;                    //[?] Ipv6 is cpmpatible with Ipv4.
  inet_pton(AF_INET6, ip_str, &addr.sin6_addr);		// Remote memory pool is waiting on 10.0.0.2:9400.
  addr.sin6_port = htons(atoi(port_str));

  assert((ec = rdma_create_event_channel()) != NULL, "rdma_create_event_channel failed.");
  assert(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP) == 0, "rdma_create_id failed.");
  //assert(rdma_bind_addr(listener, (struct sockaddr *)&addr) == 0, "rdma_bind_addr failed :return non-zero ");
  if(rdma_bind_addr(listener, (struct sockaddr *)&addr) != 0){
    // Bind ip:port failed
    perror("rdma_bind_addr failed.");
    assert(0,"rdma_bind_addr failed.");  // trigger vm_error report.
  }
  assert(rdma_listen(listener, 10) == 0, "rdma_listen failed,return non-zero"); 						/* backlog=10 is arbitrary */
  port = ntohs(rdma_get_src_port(listener));

  tty->print("listening on port %d.\n", port);

  //
  // [?] 1) How to bind the global_rdma_ctx with these created rdma_cm_id, rdma_event_channel ??
  //     2) How to bind event->id with rdma_cm_id ?
	// 
	// Initialize the whole heap as RDMA buffer. 	
	//	In our design, we map the whole Java heap RDMA buffer directly.
	//
  global_rdma_ctx = (struct context *)malloc(sizeof(struct context));
	init_memory_pool(heap_start, heap_size, global_rdma_ctx);

  //
  // [?] Not like kernel level client running in a notify mode, here has to poll the cm event manually ?
  //	Current process stops here to wait for RDMA signal. 
	//
  while (rdma_get_cm_event(ec, &event) == 0) {
    struct rdma_cm_event event_copy;

    memcpy(&event_copy, event, sizeof(*event));
    rdma_ack_cm_event(event);    		// [?] Why do we free the event here ?

    if (on_cm_event(&event_copy))   // RDMA communication event handler. 
      break;
  }

	// Reach here only when get WRONG rdma messages ?
	//
	tty->print("%s, RDMA cma thread exit. \n",__func__);
 // rdma_destroy_id(listener);
  rdma_destroy_event_channel(ec);

  return NULL;
}


/**
 * Utilize Java heap information to initialize RDMA memory pool information.
 * Delay the RDMA buffer registration untile RDMA connection is built.
 */
void init_memory_pool(char* heap_start, size_t heap_size, struct context* rdma_ctx ){

	int i;
	rdma_ctx->mem_pool = (struct rdma_mem_pool* )malloc(sizeof(struct rdma_mem_pool));

	#ifdef DEBUG_RDMA_SERVER
	  // heap size should be in GB granularity.
	  if(heap_size % ONE_GB)
		  goto err;

	  // heap size should be small than the hard limitations.
	  if(heap_size/ONE_GB > MAX_FREE_MEM_GB )
		  goto err;

	#endif

	rdma_ctx->mem_pool->Java_heap_start = heap_start;

	// Initialize the status of Region 
	// Divide the heap into multiple Regions.
	rdma_ctx->mem_pool->region_num = heap_size/ONE_GB/REGION_SIZE_GB;

  rdma_ctx->mem_pool->region_list[0]  = heap_start;
	for(i=1;i<rdma_ctx->mem_pool->region_num ;i++){
		rdma_ctx->mem_pool->region_list[i]  = rdma_ctx->mem_pool->region_list[i-1] + (size_t)REGION_SIZE_GB*ONE_GB;  // Not exceed the int limitation.
		rdma_ctx->mem_pool->cache_status[i] = -1;  // -1 means not bind  to CPU server.

    #ifdef DEBUG_RDMA_SERVER
    printf("%s, Record memory Region[%d] : 0x%llx \n", __func__, i, (unsigned long long)rdma_ctx->mem_pool->region_list[i] );
    #endif
	}


	#ifdef DEBUG_RDMA_SERVER
	tty->print("Registered %llu GB (whole head) as RDMA Buffer \n", (unsigned long long)heap_size/ONE_GB);
	#endif

	// Register the whole Java heap as RDMA buffer.
	// Because we need to bind the RDMA  buffers with RDMA QP. 
	// Wait until the RDMA context is intialized, then doing the RDMA buffer registration.

	return;

	err:
	tty->print("ERROR in %s \n", __func__);
}


/**
 * Handle the communication(CM) event.
 *    a. Accept the RDMA connection request from client.
 *    b. Sent memory free size to client. 
 *    DONE.
 * 
 * More explanation
 *    Self defined behavior, for these RDMA CM event, send some RDMA WR back to caller.
 *    The caller has to post a receive WR to receive these WR ?
 * 
 * 		[?] struct rdma_cm_event ?  Where is the definition ?
 * 
 */
int on_cm_event(struct rdma_cm_event *event){
  int r = 0;

  if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST){    // 1) ACCEPT the RDMA conenct request from client.

    #ifdef DEBUG_RDMA_SERVER
    tty->print("Get RDMA_CM_EVENT_CONNECT_REQUEST \n");
    #endif

    r = on_connect_request(event->id);                  //    event->id : rdma_cm_id 
  }else if (event->event == RDMA_CM_EVENT_ESTABLISHED){   // 2) After ACCEPT the connect request, server will get a RDMA_CM_EVENT_ESTABLISHED ack?
    
    #ifdef DEBUG_RDMA_SERVER
    tty->print("Get RDMA_CM_EVENT_ESTABLISHED \n");
    #endif
    
    r = rdma_connected(event->id);                       //    send the free memory to client size of current server.
  }else if (event->event == RDMA_CM_EVENT_DISCONNECTED){

    #ifdef DEBUG_RDMA_SERVER
    tty->print("Get RDMA_CM_EVENT_DISCONNECTED \n");
    #endif

    r = on_disconnect(event->id);
  }else{
    die("on_cm_event: unknown event.");
  }

  return r;
}



/**
 * CPU server send a reques to build a RDMA connection.   
 * ACCEPT the RDMA conenction.
 * 
 * rdma_cm_id : is listening on the Ip of the IB.
 * 
 */
int on_connect_request(struct rdma_cm_id *id)
{
  struct rdma_conn_param cm_params;

  tty->print("%s, received connection request.\n", __func__);
  build_connection(id);					// Build the RDMA connection.
  build_params(&cm_params);			// [?] Set some RDMA paramters. 
  TEST_NZ(rdma_accept(id, &cm_params));  // ACCEPT the request to build RDMA connection.
  tty->print("%s, Send ACCEPT back to CPU server \n", __func__);

  return 0;
}



/**
 * Already intialized hardware information : cm_id,  communication event_channel, listen, ip:port.
 * Build the RDMA connection required structures: 
 * 			RDMA QP, CQ, RDMA messages buffer and a daemon thread, poll_cq, wait for events. 
 * 
 * 
 * 
 * 
 *  More Explanation
 * 		We already initialized the IB hardware, ib, 
 *    rdma_cm_id->verbs is ibv_context. used for mellonax context.
 *    rdma_cm_id->context : self defined driver data
 * 		
 * 		Build RDMA connection by using the global global_rdma_ctx.
 */
void build_connection(struct rdma_cm_id *id){
  int i;
  //struct connection *conn;
  struct ibv_qp_init_attr qp_attr;

  // 1) build a listening daemon thread 
  build_context(id->verbs);  // create a daemon thread, poll_cq
  build_qp_attr(&qp_attr);   // Initialize qp_attr

  TEST_NZ(rdma_create_qp(id, global_rdma_ctx->pd, &qp_attr));   // Build the QP.

  // 2) Build the acknowledge RDMA packages.
  //id->context = conn = (struct connection *)malloc(sizeof(struct connection));
  id->context = global_rdma_ctx;  // Assign self-defined context to rdma_cm_id->context. Then we can get it from wc->wr_id.

  global_rdma_ctx->id = id;
  global_rdma_ctx->qp = id->qp;

  global_rdma_ctx->send_state = SS_INIT;    // [?] Do ever use these states ?
  global_rdma_ctx->recv_state = RS_INIT;
  global_rdma_ctx->server_state = S_WAIT;

  global_rdma_ctx->connected = 0;
// 	atomic_init(&global_rdma_ctx->cq_qp_state);
//  atomic_set(&global_rdma_ctx->cq_qp_state, CQ_QP_BUSY);  // [?] Need to set it idle, after the connection is built
 // global_rdma_ctx->free_mem_gb = 0;

//  sem_init(&global_rdma_ctx->stop_sem, 0, 0);
 // sem_init(&global_rdma_ctx->evict_sem, 0, 0);
  //global_rdma_ctx->sess = &session;

  //for (i = 0; i < MAX_FREE_MEM_GB; i++){
  //  conn->sess_chunk_map[i] = -1;
  //}

	// global_rdma_ctx->mapped_chunk_size = 0;

  //add to session 
  // for (i=0; i<MAX_CLIENT; i++){
  //   if (session.conns_state[i] == CONN_IDLE){
  //     session.conns[i] = conn;
  //     session.conns_state[i] = CONN_CONNECTED;    // Mark this session connected ? before do the RDMA connection ?
  //     conn->conn_index = i;
  //     break;
  //   } 
  // }
  //session.conn_num += 1;

  register_rdma_comm_buffer(global_rdma_ctx);  // Register RDMA message memory regions, recv/send.

  // Send a waiting receive WR.
  // Post the receive wr before ACCEPT the RDMA connection.
  post_receives(global_rdma_ctx);    
}



/**
 * Build a daemon thread,poll_cq(void *ctx), to handle the 2-sided RDMA communication.
 * The 2-sided RDMA message is used to build the connection with CPU server.
 * After this, CPU server uses 1-sided RDMA read/write to access the memory pool in current Memory server.	   
 * 
 * Create : pd, rdma_channel, cq here.
 * 
 * Parametsers :
 * 		ibv_context : rdma_cm_id->verbs, IB hardware descriptor.
 * 		
 */
void build_context(struct ibv_context *verbs)  // rdma_cm_id->verbs
{

  // if (global_rdma_ctx) {  		// Confirm this is the first time to initialize global_rdma_ctx.
  //   if (global_rdma_ctx->ctx != verbs)
  //     die("cannot handle events in more than one context.");

  //   return;
  // }

  //
  //  [!!] Have to malloc its instance ealier, in Main function.
  //global_rdma_ctx = (struct context *)malloc(sizeof(struct context));

  global_rdma_ctx->ctx = verbs;   // struct context->ibv_context.
  TEST_Z(global_rdma_ctx->pd = ibv_alloc_pd(global_rdma_ctx->ctx));
  TEST_Z(global_rdma_ctx->comp_channel = ibv_create_comp_channel(global_rdma_ctx->ctx));

	// Parameters of ibv_create_cq :
	//		struct ibv_context *context, 	// IB hardware context, 
	//		int cqe,  										// Number of Completion Queue Entries, then we can  poll WC from the cq. [?] We poll CQ one by one?
	//		void *cq_context, 						// NULL, 
	//		struct ibv_comp_channel *channel, 	
	//		int comp_vector								// 0,
	//
  TEST_Z(global_rdma_ctx->cq = ibv_create_cq(global_rdma_ctx->ctx, 10, NULL, global_rdma_ctx->comp_channel, 0)); /* cqe=10 is arbitrary */
  TEST_NZ(ibv_req_notify_cq(global_rdma_ctx->cq, 0));			// , solicited_only == 0, means give a notification for any WC.

	// Thread : global_rdma_ctx->cq_poller_thread,
	// Thread attributes : NULL
	// Thread main routine : poll_cq(void *), 
	// Thread parametes : NULL
	//	[?] How to Start and Quit the execution of this method ?  
	//
  TEST_NZ(pthread_create(&global_rdma_ctx->cq_poller_thread, NULL, poll_cq, NULL));  // [?] Busy polling. 
}


/**
 * Build qp based on global_RDMA_context.
 */
void build_qp_attr(struct ibv_qp_init_attr *qp_attr)
{
  memset(qp_attr, 0, sizeof(*qp_attr));

  qp_attr->send_cq = global_rdma_ctx->cq;  // Both Send/Recv queue share the same cq.
  qp_attr->recv_cq = global_rdma_ctx->cq;
  qp_attr->qp_type = IBV_QPT_RC;		// QP type, Reliable Communication.

  qp_attr->cap.max_send_wr = 10;
  qp_attr->cap.max_recv_wr = 100; //original 10  ??
  qp_attr->cap.max_send_sge = MAX_REQUEST_SGL;    // enable  the scatter/gather
  qp_attr->cap.max_recv_sge = MAX_REQUEST_SGL;
}


/**
 * 	A deamon thread who is busy polling the CQ.
 *  Handle the two-sided RDMA messages
 * 	
 * 	The paramter is NULL at present. 
 * 
 */
void * poll_cq(void *ctx)
{
  struct ibv_cq *cq;  //
  struct ibv_wc wc;		// 

  while (1) {
    TEST_NZ(ibv_get_cq_event(global_rdma_ctx->comp_channel, &cq, &ctx));
    ibv_ack_cq_events(cq, 1);						// [?] work with mutex, heavy ? 
    TEST_NZ(ibv_req_notify_cq(cq, 0));	// Should use global_rdma_ctx->cq ??

    while (ibv_poll_cq(cq, 1, &wc))   // If here busily polls the CQ, no need to use the ibv_req_notify_cq ?
      handle_cqe(&wc);
  }

  return NULL;
}





/**
 * Build and register the 2-sided RDMA buffers.  
 *  a. DMA buffer, user level.
 *      context->send_msg/recv_msg 
 */
void register_rdma_comm_buffer(struct context *rdma_ctx)
{
  rdma_ctx->send_msg = (struct message *)malloc(sizeof(struct message));   // 2-sided RDMA messages
  rdma_ctx->recv_msg = (struct message *)malloc(sizeof(struct message));

	// [?] Is the the 1-sided RDMA buffer ?
  TEST_Z(rdma_ctx->send_mr = ibv_reg_mr(
    rdma_ctx->pd, 						// protect domain 
    rdma_ctx->send_msg, 			// start address
    sizeof(struct message),   // Register the send_msg/recv_msg as 1-sided RDMA buffer.
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));

  TEST_Z(rdma_ctx->recv_mr = ibv_reg_mr(
    rdma_ctx->pd, 
    rdma_ctx->recv_msg, 
    sizeof(struct message),   //
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));

}

/**
 * Post a receive WR to wait for RDMA message.
 *    This is an empty receive WR, waiting for the data sent from client.
 */
void post_receives(struct context *rdma_ctx)
{
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  wr.wr_id    = (uintptr_t)rdma_ctx;
  wr.next     = NULL;
  wr.sg_list  = &sge;
  wr.num_sge  = 1;					// [?] Why does the number of sge for each WR is always 1 ??

  sge.addr    = (uintptr_t)rdma_ctx->recv_msg;   // Put a recv_wr to wait for 2-sided RDMA message.
  sge.length  = sizeof(struct message);
  sge.lkey    = rdma_ctx->recv_mr->lkey;         // For message receive, use the lkey of receive RDMA MR. 

  TEST_NZ(ibv_post_recv(rdma_ctx->qp, &wr, &bad_wr)); // post a recv wait for WR.
}


//
// 1.2 Build the RDMA parameters 
//
void build_params(struct rdma_conn_param *params)
{
  memset(params, 0, sizeof(*params));

  params->initiator_depth = params->responder_resources = 1;
  params->rnr_retry_count = 7; /* infinite retry */
}




//
// <<<<<<<<<<<<<<<<<<<<<<<  End of RDMA initialization <<<<<<<<<<<<<<<<<<<<<<<
//







//
// >>>>>>>>>>>>>>>>>>>>>>  Start of sending 2-sided RDMA message to CPU server >>>>>>>>>>>>>>>>>>>>>>
//


/**
 * Get a WC from CQ, time to handle the recr_wr.
 * 
 */
void handle_cqe(struct ibv_wc *wc){

  // wc->wr_id is a reserved viod* pointer for any self-attached context.
  // context->recv_msg is the binded DMA buffer.
  //
  struct context *rdma_ctx = (struct context *)(uintptr_t)wc->wr_id;

  if (wc->status != IBV_WC_SUCCESS)
    die("handle_cqe: status is not IBV_WC_SUCCESS.");

  if (wc->opcode == IBV_WC_RECV){ //Recv
    switch (rdma_ctx->recv_msg->type){    // Check the DMA buffer of recevei WR.
      case QUERY:
        tty->print("%s, QUERY \n", __func__);
        //atomic_set(&rdma_ctx->cq_qp_state, CQ_QP_BUSY);
        send_free_mem_size(rdma_ctx);				//[!] This is the first time send the FREE SIZE information
        post_receives(rdma_ctx);
        break;
      case REQUEST_CHUNKS:          //client requests for multiple memory chunks from current server.
        tty->print("%s, REQUEST_CHUNKS, Send available Regions to CPU server \n", __func__);
        //atomic_set(&rdma_ctx->cq_qp_state, CQ_QP_BUSY);
        // Send all the available Regions to CPU
        rdma_ctx->server_state = S_BIND;
				send_regions(rdma_ctx);
        // post a recv wr to wait for responds.
        post_receives(rdma_ctx);

        //allocate n chunks, and send to client
      //  send_chunks_to_client(rdma_ctx, rdma_ctx->recv_msg->size_gb);
      //  session.conns_state[rdma_ctx->conn_index] = CONN_MAPPED;
      //  post_receives(rdma_ctx);
        break;
      case REQUEST_SINGLE_CHUNK:    // client requests for single memory chunk from this server. Usually used for debuging.

        tty->print("%s, BIND_SINGLE, Send a single Region to CPU server\n", __func__);
        //atomic_set(&rdma_ctx->cq_qp_state, CQ_QP_BUSY);
        rdma_ctx->server_state = S_BIND;
        //allocate n chunks, and send to client
      //  send_single_chunk_to_client(conn, conn->recv_msg->size_gb); // [?]  2nd parameter should be, int client_chunk_index ??
      //  session.conns_state[conn->conn_index] = CONN_MAPPED;  // [?] The connection->conn_index is the index of the chunk to be mapped ??
      //  post_receives(conn);
        break;
      case ACTIVITY:
        tty->print("%s, ACTIVITY \n", __func__);
        //copy bitmap data
      //  sem_post(&conn->evict_sem);
        break; 
      case DONE:
        tty->print("%s, DONE \n", __func__);
        //atomic_set(&rdma_ctx->cq_qp_state, CQ_QP_BUSY);
      //  recv_done(conn); //post receive inside
        break;
      default:
        die("unknow received message\n");
    }
  }else{ 
			//Send a RDMA message ? 
			#ifdef DEBUG_RDMA_SERVER
			tty->print("%s, Send a RDMA message done. \n",__func__);
			#endif
      //atomic_set(&rdma_ctx->cq_qp_state, CQ_QP_IDLE);
		
  }
}







/**
 *  After accept the RDMA connection from client.
 *  1) Register the whole heap as RDMA buffer.
 *  2) Send the free size of this server immediately. 
 * 				The client has to post a receive WR waiting for this already.
 * 	
 * More explanation:
 * 		In our design, the Java heap in Memory pool can expand. 
 * 		CPU server needs to send the message to expand the memory pool in Memory server. 
 * 		All the Memory servers expand at the same time and at same ratio. 
 * 		After expantion, Memory server to register all the newaly allocated Regions to CPU server.
 */
int rdma_connected(struct rdma_cm_id *id){
	int i;

  // RDMA connection is build.
	struct context* rdma_ctx = (struct context*)id->context;
	rdma_ctx->connected = 1;
	tty->print("%s, connection build\n", __func__);

	// Register the RDMA buffer here
	// We need to send the registered virtual address to CPU server in "send_free_mem_size"
	// [?] Can we just register the whole heap once ?
	// Or we have to register the Java heap in a Region granularity. 

	// Choice #1, register the whole Java heap.
	// rdma_ctx->mem_pool->Java_heap_mr = ibv_reg_mr(rdma_ctx->pd, rdma_ctx->mem_pool->Java_start,rdma_ctx->mem_pool->size_gb*ONE_GB,
	// 																																														IBV_ACCESS_LOCAL_WRITE | 
  //                                                                                             IBV_ACCESS_REMOTE_WRITE | 
  //                                                                                             IBV_ACCESS_REMOTE_READ);

	// Choice #2, register the Region one by one.
  //  This design is easy to handle the Memory pool scale. 
	for(i=0; i< rdma_ctx->mem_pool->region_num; i++){
    
		rdma_ctx->mem_pool->Java_heap_mr[i] = ibv_reg_mr(rdma_ctx->pd, rdma_ctx->mem_pool->region_list[i], REGION_SIZE_GB*ONE_GB,
	 																																															IBV_ACCESS_LOCAL_WRITE | 
                                                                                               IBV_ACCESS_REMOTE_WRITE | 
                                                                                               IBV_ACCESS_REMOTE_READ);
  
    #ifdef DEBUG_RDMA_SERVER
    tty->print("Register Region[%d] : 0x%llx to RDMA Buffer[%d] : 0x%llx, rkey: 0x%llx  done \n", i, 
                                                                              (unsigned long long)rdma_ctx->mem_pool->region_list[i],
                                                                              i, 
                                                                              (unsigned long long)rdma_ctx->mem_pool->Java_heap_mr[i],
                                                                              (unsigned long long)rdma_ctx->mem_pool->Java_heap_mr[i]->rkey);
    #endif
  
  }


  // Send the available free size to CPU server to let it prepare necessary meta data.
  //
  send_free_mem_size(rdma_ctx);

  return 0;
}


/**
 * Post a two-sided RDMA message to client to inform the free memory size in server.
 * 	
 * 	1) Inform CPU server the available memory size at present.
 * 	
 */
void send_free_mem_size(struct context* rdma_ctx){
	int i;

	rdma_ctx->send_msg->size_gb = rdma_ctx->mem_pool->region_num * REGION_SIZE_GB;  // Total heap size in GB.
	
	for(i=0; i<rdma_ctx->mem_pool->region_num; i++ ){
		rdma_ctx->send_msg->buf[i]	= 0x0;
		rdma_ctx->send_msg->rkey[i]	=	0x0;  // The contend tag of the RDMA message.
	}

  rdma_ctx->send_msg->type = FREE_SIZE;			// Need to modify the CPU server behavior.
  tty->print("%s , Send free memory information to CPU server, %d GB \n", __func__, rdma_ctx->send_msg->size_gb);
  send_message(rdma_ctx);
}


/**
 * Bind the available Regions as RDMA buffer to CPU server
 * 
 * 1) Send the registered RDMA buffer, ie the whole Jave heap for FREE_SIZE, to CPU server.
 */
void send_regions(struct context* rdma_ctx){
	int i;
	rdma_ctx->send_msg->size_gb = rdma_ctx->mem_pool->region_num * REGION_SIZE_GB;  // Total heap size in GB.
	
	for(i=0; i<rdma_ctx->mem_pool->region_num; i++ ){
		rdma_ctx->send_msg->buf[i]	= (uint64_t)rdma_ctx->mem_pool->Java_heap_mr[i]->addr;
		rdma_ctx->send_msg->rkey[i]	=	rdma_ctx->mem_pool->Java_heap_mr[i]->rkey;
	}

  rdma_ctx->send_msg->type = SEND_CHUNKS;			// Need to modify the CPU server behavior.
  tty->print("%s , Send registered Java heap to CPU server, %d GB \n", __func__, rdma_ctx->send_msg->size_gb);
  send_message(rdma_ctx);
}



/**
 * Do the 2-sided RDMA send operation.
 * 
 * 	Use the reserved,  context->send_msg, to send the 2-sided RDMA message.
 * 	Make sure the message data have been inserted into send_msg before invoking this function.
 */
void send_message(struct context * rdma_ctx){
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));

  wr.wr_id = (uintptr_t)rdma_ctx;		// Attch the struct context as Self-defined context ? Wast too much bandwidth ?
  wr.opcode = IBV_WR_SEND;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;

  sge.addr = (uintptr_t)rdma_ctx->send_msg;
  sge.length = sizeof(struct message);
  tty->print("%s, message size = %lu\n", __func__, sizeof(struct message));
  sge.lkey = rdma_ctx->send_mr->lkey;

  while (!rdma_ctx->connected);  // Wait until RDMA connection is built.

  TEST_NZ(ibv_post_send(rdma_ctx->qp, &wr, &bad_wr));
}




//
// <<<<<<<<<<<<<<<<<<<<<<<  End of sending 2-sided RDMA message to CPU server <<<<<<<<<<<<<<<<<<<<<<<
//





//
// >>>>>>>>>>>>>>>>>>>>>>  Start of Resource collection >>>>>>>>>>>>>>>>>>>>>>
//
// Design Logic
//	This Memory server is dedicated for one JVM instance on CPU server.
//	If the JVM instance on CPU server exit, the Memory server has to exit, too.
//

int on_disconnect(struct rdma_cm_id *id)
{
  tty->print("%s, disconnect current RDMA connection.\n", __func__);

  destroy_connection((struct context*)id->context);
  return 0;
}

/**
 * Free this RDMA connection related resource.
 */
void destroy_connection(struct context* rdma_ctx)
{

  int i = 0;
  int index;
  rdma_destroy_qp(rdma_ctx->id);

  ibv_dereg_mr(rdma_ctx->send_mr);
  ibv_dereg_mr(rdma_ctx->recv_mr);
  free(rdma_ctx->send_msg);
  free(rdma_ctx->recv_msg);

	// All the Regions should be freed.
  for (i=0; i<rdma_ctx->mem_pool->region_num; i++){
    if (rdma_ctx->mem_pool->Java_heap_mr[i] == NULL) {
      continue;   // Not rereigser this Region for now.
    }
    
		ibv_dereg_mr(rdma_ctx->mem_pool->Java_heap_mr[i]);
  }

  rdma_destroy_id(rdma_ctx->id);
	//rdma_destroy_event_channel(ec);

	// Free all the RDMA related context variables
	//free(global_mem_pool);
	free(global_rdma_ctx);

  // Exit the Java instance.
  tty->print("%s, Exit Memory Server JVM Instance. \n", __func__);
  vm_direct_exit(0);  // 0 : normally exit.
}



//
// <<<<<<<<<<<<<<<<<<<<<<<  End of Resource collection  <<<<<<<<<<<<<<<<<<<<<<<
//


// Tools


void die(const char *reason){
  tty->print("ERROR, %s\n", reason);
  exit(EXIT_FAILURE);
}
