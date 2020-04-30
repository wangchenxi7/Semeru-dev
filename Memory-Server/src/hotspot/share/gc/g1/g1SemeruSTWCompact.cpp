#include "gc/g1/g1SemeruSTWCompact.inline.hpp"

// Have to use some G1SemeruConcurrentMark's structure
#include "gc/g1/g1SemeruConcurrentMark.hpp"






/**
 * Semeru MS - Constructor of the G1SemeruSTWCompact
 * 	
 * [X] Share all the G1SemeruConcurrentMark's concurrent thread source. 
 * 		 But define its own code to be executed.
 * 		 So must initialize this instance after initialize G1SemeruConcurrentMark, _semeru_cm.
 * 
 */
G1SemeruSTWCompact::G1SemeruSTWCompact(G1SemeruCollectedHeap* 	g1h,
																	 		 G1SemeruConcurrentMark* 	semeru_cm) :
	_semeru_cm_thread(semeru_cm->_semeru_cm_thread),	// shared with CM
	_semeru_h(g1h),
	_completed_initialization(false),
	_mem_server_cset(&(semeru_cm->_mem_server_cset)),		// [XX] Filled by Memory Server.
	_max_num_tasks(semeru_cm->_max_num_tasks),				// shared with CM
	_num_active_tasks(semeru_cm->_num_active_tasks),	// shared with CM
	//_tasks(NULL),							// code task
	_concurrent(false),
	_has_aborted(false),
	_compaction_points(NULL),
	_gc_timer_cm(new (ResourceObj::C_HEAP, mtGC) ConcurrentGCTimer()),
	_gc_tracer_cm(new (ResourceObj::C_HEAP, mtGC) G1OldTracer()),

	// _verbose_level set below
	_init_times(),
	_remark_times(),
	_remark_mark_times(),
	_remark_weak_ref_times(),
	_cleanup_times(),
	_total_cleanup_time(0.0),
	_accum_task_vtime(semeru_cm->_accum_task_vtime),

	// concurrent thread resources
	_concurrent_workers(semeru_cm->_concurrent_workers),
	_num_concurrent_workers(semeru_cm->_num_concurrent_workers),
	_max_concurrent_workers(semeru_cm->_max_concurrent_workers)
{

	// 1) Build the _mem_server_cset

	// The Semeru Memory Server Scanned CSet is shared with G1SemeruConcurrentMark->_mem_server_cset


	// 2) Build the G1SemeruSTWCompactTask code .

	// Build the tasks executed by each worker.
	// Does the _tasks[] also include the ParallelThread ?
	// Seems no. This _tasks is only for SemeruConcurrentGC worker.
	// _tasks = NEW_C_HEAP_ARRAY(G1SemeruSTWTask*, _max_num_tasks, mtGC);			
	_accum_task_vtime = NEW_C_HEAP_ARRAY(double, _max_num_tasks, mtGC);

	// so that the assertion in MarkingTaskQueue::task_queue doesn't fail
	_num_active_tasks = _max_num_tasks;

	// G1SemeruSTWTask doesn't need the StarTask queue
	for (uint i = 0; i < _max_num_tasks; ++i) {

		// worker_id, G1SemeruSTWCompact
		// _tasks[i] = new G1SemeruSTWTask(i, this);
		_accum_task_vtime[i] = 0.0;
	}




	// Compaction fields
	//
	
	// Destination Regions for Compaction.
	_compaction_points = NEW_C_HEAP_ARRAY(G1SemeruCompactionPoint*, _max_num_tasks, mtGC);
  for (uint i = 0; i < _max_num_tasks; i++) {
    _compaction_points[i] = new G1SemeruCompactionPoint();
  }



	// Initialization of G1SemeruSTWCompact is done.
	_completed_initialization = true;

}







/**
 * Semeru Memory Server : Cliam server Regions for comaction.
 * 	There are some constraints for the compacted Region cliaming.  
 *  1) Merge the Rgions with many cross-region refences together.
 * 		[?] How to implement this ??
 * 			Select the one with most cross-region referenced scanned region of the las compact Region.
 * 
 */
SemeruHeapRegion*
G1SemeruSTWCompact::claim_region_for_comapct(uint worker_id, SemeruHeapRegion* prev_compact) {

	SemeruHeapRegion* curr_region	= NULL;

	#ifdef ASSERT
	if(_mem_server_cset == NULL){
		tty->print("%s, Memory Server Scanned CSet is NULL. \n",__func__);
		curr_region = NULL;
		goto err;
	}
	#endif

	//G1SemeruCMCSetRegions* mem_server_cset = this->mem_server_cset();
	// No need  to check here.

	do{

		// claim a already scanned Region by CM
		//
		// [XXX] who has the most cross-region references with prev_compact region ??
		//
		curr_region	=	_mem_server_cset->claim_cm_scanned_next();


		// Cliam a Region successfully
		if( curr_region != NULL ){
			return curr_region;
		} // end of curr_region != NULL
	
	}while(curr_region != NULL);


err:
	return curr_region; // run out of Memory Server CSet regions.
}










/**
 * Semeru MS - Trigger the compact
 * 		
 * 1) Compact the scanned Region of memory server CSet.	Ø
 * 2) Evacuate alive objects marked in SemeruHeapRegion's alive_bitmap.
 * 
 */
void G1SemeruSTWCompact::semeru_stw_compact() {
	//_restart_for_overflow = false;		// freshly scan, not Remark

	_num_concurrent_workers = _num_active_tasks;  // This value is gotten from G1SemeruConcurrentMark

	uint active_workers = MAX2(1U, _num_concurrent_workers);

	// Setting active workers is not guaranteed since fewer
	// worker threads may currently exist and more may not be
	// available.
	active_workers = _concurrent_workers->update_active_workers(active_workers);
	log_info(semeru, mem_compact)("Using %u workers of %u for Semeru MS compacting", active_workers, _concurrent_workers->total_workers());

	// Parallel task terminator is set in "set_concurrency_and_phase()"
	set_concurrency_and_phase(active_workers, true /* concurrent */);  // actually here is executed in STW.

	G1SemeruSTWCompactTask compacting_task(this);  		// Invoke the G1SemeruSTWCompactTask WorkGang to run.
	_concurrent_workers->run_task(&compacting_task);		// STWCompact share ConcurrentMark's concurrent workers.
	print_stats();
}



void G1SemeruSTWCompact::set_concurrency_and_phase(uint active_tasks, bool concurrent) {
	set_concurrency(active_tasks);

	_concurrent = concurrent;		// g1SemeruConcurrentMark->_concurrent specify which phase we are : CM or Remark.

	if (!concurrent) {
		// At this point we should be in a STW phase, and completed marking.
		assert_at_safepoint_on_vm_thread();
		assert(out_of_regions(),"%s, out_of_regions() check failed. \n",__func__ );
	}
}




/**
 * [?] What's the terminator used for ?
 *  
 */
void G1SemeruSTWCompact::set_concurrency(uint active_tasks) {
	assert(active_tasks <= _max_num_tasks, "we should not have more");

	_num_active_tasks = active_tasks;

	// // Need to update the three data structures below according to the
	// // number of active threads for this phase.
	// // [?] What's the connection between active_task and _task_queus ?
	// _terminator = TaskTerminator((int) active_tasks, _task_queues); 
	
	// _first_overflow_barrier_sync.set_n_workers((int) active_tasks);
	// _second_overflow_barrier_sync.set_n_workers((int) active_tasks);
}












//
// G1SemeruSTWtask
//



// G1SemeruSTWTask::G1SemeruSTWTask(uint worker_id,
// 									 G1SemeruSTWCompact* sc) :
// 	_worker_id(worker_id),
// 	_semeru_h(sc->_semeru_h),
// 	_semeru_sc(sc),
// 	_alive_bitmap(NULL),
// 	_curr_compacting_region(NULL),
// 	_has_aborted(false),
// 	_has_timed_out(false),
// 	_step_times_ms(),
// 	_elapsed_time_ms(0.0),
// 	_termination_time_ms(0.0),
// 	_termination_start_time_ms(0.0),
// 	_calls(0),
// 	_time_target_ms(0.0),
// 	_start_time_ms(0.0)
// {


// 	#ifdef ASSERT
// 	log_debug(semeru, thread)("%s, build the G1SemeruSTWtask[%x]. \n", __func__, worker_id );
// 	#endif
// }




// void G1SemeruSTWTask::print_stats() {
// 	log_debug(gc, stats)("Marking Stats, task = %u, calls = %u", _worker_id, _calls);
// 	log_debug(gc, stats)("  Elapsed time = %1.2lfms, Termination time = %1.2lfms",
// 											 _elapsed_time_ms, _termination_time_ms);
// 	log_debug(gc, stats)("  Step Times (cum): num = %d, avg = %1.2lfms, sd = %1.2lfms max = %1.2lfms, total = %1.2lfms",
// 											 _step_times_ms.num(),
// 											 _step_times_ms.avg(),
// 											 _step_times_ms.sd(),
// 											 _step_times_ms.maximum(),
// 											 _step_times_ms.sum());
// 	// size_t const hits = _mark_stats_cache.hits();
// 	// size_t const misses = _mark_stats_cache.misses();
// 	// log_debug(gc, stats)("  Mark Stats Cache: hits " SIZE_FORMAT " misses " SIZE_FORMAT " ratio %.3f",
// 	// 										 hits, misses, percent_of(hits, hits + misses));
// }






// bool G1SemeruSTWTask::should_exit_termination() {
// 	// if (!regular_clock_call()) {
// 	// 	return true;
// 	// }

// 	// This is called when we are in the termination protocol. We should
// 	// quit if, for some reason, this task wants to abort or the global
// 	// stack is not empty (this means that we can get work from it).
// 	return _semeru_sc->out_of_scanned_cset() || has_aborted();
// }





//
// G1SemeruSTWCompactTask 
//




	/**
	 * Semeru MS - WorkGang, apply compaction closure to the scanned Regions.
	 * 
	 * 	1) this worker is scheduled form GangWorker::run_task(WorkData data)
	 *  
	 */
	void  G1SemeruSTWCompactTask::work(uint worker_id) {
		assert(Thread::current()->is_ConcurrentGC_thread(), "Not a concurrent GC thread");
		ResourceMark rm;			// [?] What's this resource used for ?
		double start_vtime = os::elapsedVTime();
		bool is_cpu_server_in_stw = false;

		_worker_id = worker_id;
		_cp = _semeru_sc->compaction_point(_worker_id); // [X] Remember to reset the compaction_point at the end of this work.
		SemeruHeapRegion* region_to_evacuate = NULL;

		log_debug(semeru, mem_compact)("%s, Enter SemeruSWTCompact worker[%x] \n", __func__, worker_id);



		// Claim scanned Regions from the MS CSet.
		do{
				// Check CPU server STW states.
				// Compact one Region at least. When we reach here, we already confirmed the cpu server STW mode.
				is_cpu_server_in_stw = _semeru_sc->_semeru_h->is_cpu_server_in_stw();

				// Start: When the compacting for a Region is started,
				// do not interrupt it until the end of compacting.

				region_to_evacuate = _semeru_sc->claim_region_for_comapct(worker_id, region_to_evacuate);
				if(region_to_evacuate != NULL){
					log_debug(semeru,mem_compact)("%s, Claimed Region[0x%lx] to be evacuted.", __func__, (size_t)region_to_evacuate->hrm_index() );

					// Phase#1 Sumarize alive objects' destinazion
					// 1) put forwarding pointer in alive object's markOop
					//
					// [??] Make this phase Concurrent ??
					//
					phase1_prepare_for_compact(region_to_evacuate);

					// Phase#2 Adjust object's intra-Region feild pointer
					// The adjustment is based on forwarding pointer.
					// This has to be finished bofore data copy, which may overwrite the original alive objects and their forwarding pointer.
					phase2_adjust_intra_region_pointer(region_to_evacuate);

					// Phase#2.1
					// Record the new address for the objects in target_obj_queue
					// Only these objects are cross-region referenced. 
					// Their new addr is stored in the markOop for now.
					record_new_addr_for_target_obj(region_to_evacuate);

	

					// Phase#3 Do the compaction
					// Multiple worker threads do this parallelly
					phase3_compact_region(region_to_evacuate);


					// Phase#4 Inter-Region reference fields update.
					// Should adjust the inter-region reference before do the compaction.
					// 1) The target Region is in Memory Server CSet
					// 2) The target Region is in Another Server(CPU server, or another Memory server)

					// TO BE DONE.
					//
					// Synchronized with other servers, CPU server or Memory server, to get the cross-region-ref-update queue.
					//


					phase4_adjust_inter_region_pointer(region_to_evacuate);

					

					log_debug(semeru,mem_compact)("%s, Evacuation for Region[0x%lx] is done.", __func__, (size_t)region_to_evacuate->hrm_index() );
				}

			// End: Check if we need to stop the compacting work
			// and switch to the inter-region fields update phase.
		}while( is_cpu_server_in_stw && region_to_evacuate != NULL && !_semeru_sc->has_aborted() );


		//
		// Finish of current Compaction window.
		// 2 posible conditions:
		// 1) The CPU STW window is closed.
		// 2) All the scanned Regions are processed.
		// 3) The CPU server is changed to none STW mode. 
		assert(_semeru_sc->mem_server_scanned_cset()->is_compact_finished() || is_cpu_server_in_stw == false  || _semeru_sc->has_aborted(),
										"%s, Semeru Memoery server's compaction stop unproperly. \n", __func__ );
		

		// Reset fields
		_cp->reset_compactionPoint();

		// statistics 
		double end_vtime = os::elapsedVTime();
		_semeru_sc->update_accum_task_vtime(worker_id, end_vtime - start_vtime);
	}


/**
 * Phase#1, 
 * 1) calculate the destination address for alive objects
 *    Put forwarding pointer at alive objects' markOop
 * 
 * 2) The Region is claimed in G1SemeruSTWCompactTask::work()
 * 
 * Warning : this is a per Region processing. Pass the necessary stateless structures in.
 * 					e.g. the alive_bitmap and 
 * 
 */
void G1SemeruSTWCompactTask::phase1_prepare_for_compact(SemeruHeapRegion* hr){

	log_debug(semeru, mem_compact)("%s, Enter Semeru MS Compact Phase#1, preparation, worker [0x%x] ", __func__, this->_worker_id);

	// get _cp from G1SemeruSTWCompact->compaction_point(worker_id)
	G1SemeruCalculatePointersClosure semeru_ms_prepare(_semeru_sc, hr->alive_bitmap(), _cp, &_humongous_regions_removed);  
	semeru_ms_prepare.do_heap_region(hr);

	// update compaction_top to Region's top
	_cp->update();
}



/**
 * Phase#2, Adjust intra-Region pointers
 * Record the objects who is needed to update inter-Region pointers.
 * 
 * [x] All the forwarding pointers are stored in alive objects' markOop.
 * 			No need for the CompactionPoint.
 * 
 */






/**
 * Adjust intra-Region references for a single Region.
 * 
 * [x] This class is only used in .cpp file.
 * 
 */
class G1SemeruAdjustRegionClosure : public SemeruHeapRegionClosure {
  G1SemeruSTWCompact* _semeru_sc;
  G1CMBitMap* _bitmap;
  uint _worker_id;	// for debug
	SemeruCompactTaskQueue* _inter_region_ref_queue;  // points to G1SemeruSTWCompactTask->_inter_region_ref_queue

 public:
  G1SemeruAdjustRegionClosure(G1SemeruSTWCompact* semeru_sc, G1CMBitMap* bitmap, uint worker_id, SemeruCompactTaskQueue* inter_region_ref_queue) :
    _semeru_sc(semeru_sc),
    _bitmap(bitmap), 
		_worker_id(worker_id),
		_inter_region_ref_queue(inter_region_ref_queue) { }

  bool do_heap_region(SemeruHeapRegion* r) {
    G1SemeruAdjustClosure adjust_pointer(r, _inter_region_ref_queue); // build the closure by passing down the compact queue
    if (r->is_humongous()) {
      oop obj = oop(r->humongous_start_region()->bottom());  // get the humongous object
      obj->oop_iterate(&adjust_pointer, MemRegion(r->bottom(), r->top()));    // traverse the humongous object's fields.
    } else if (r->is_open_archive()) {
      // Only adjust the open archive regions, the closed ones
      // never change.
      G1SemeruAdjustLiveClosure adjust_oop(&adjust_pointer);
      r->apply_to_marked_objects(_bitmap, &adjust_oop);
      // Open archive regions will not be compacted and the marking information is
      // no longer needed. Clear it here to avoid having to do it later.
      _bitmap->clear_region(r);
    } else {
      G1SemeruAdjustLiveClosure adjust_oop(&adjust_pointer);
      r->apply_to_marked_objects(_bitmap, &adjust_oop);
    }
    return false;
  }


};



void G1SemeruSTWCompactTask::phase2_adjust_intra_region_pointer(SemeruHeapRegion* hr){

	log_debug(semeru, mem_compact)("%s, Enter Semeru MS Compact Phase#2, pointer adjustment, worker [0x%x] ", __func__, this->_worker_id);

	G1SemeruAdjustRegionClosure adjust_region( _semeru_sc, hr->alive_bitmap(), _worker_id, &_inter_region_ref_queue );
	adjust_region.do_heap_region(hr);
}




/**
 * Record the new address for the objects stored in Target_obj_queue.
 * Assumption
 * 	1) We are in a STW mode now. So no new cross-region reference will be added for this Region.
 *  2) Invoke this function after calculate and put the new addr in the alive objects header, markOop.
 *  	 After evacuation, the alive ojbects maybe override, and we get its new address.
 *  3) The Target_obj_queue is drained. 
 * 		 And all popped the objects are inserted into the _cross_region_ref_update_queue during its CM tracing procedure.
 * 			e.g. in function 
 * 
 */
void G1SemeruSTWCompactTask::record_new_addr_for_target_obj(SemeruHeapRegion* hr){

	log_debug(semeru, mem_compact)("%s, Store new address for the objects in Target_obj_queue of Region[0x%lx] , worker [0x%x] ", 
																																								__func__, (size_t)hr->hrm_index(), this->_worker_id);
	
	HashQueue* cross_region_ref_ptr = hr->cross_region_ref_update_queue();
	cross_region_ref_ptr->organize(); // sort and de-duplicate

	size_t len = cross_region_ref_ptr->length(); // new length.
	size_t i;
	ElemPair* elem_ptr;
	oop new_addr;

	for( i =0; i< len; i++){
		elem_ptr = cross_region_ref_ptr->retrieve_item(i);
		//assert(elem_ptr->from != NULL, "enqueued an empty target obj");
		// Debug
		if(elem_ptr->from == NULL){
			log_debug(semeru,mem_compact)("%s, get a NULL item[0x%lx] wrongly.", __func__, i);
			continue;
		}

		new_addr = elem_ptr->from->forwardee();
		elem_ptr->to = new_addr;
		
		//debug
		log_trace(semeru,mem_compact)("%s, store target obj[0x%lx] <old addr 0x%lx, new addr 0x%lx >",__func__, i, (size_t)elem_ptr->from, (size_t)new_addr );
	}// end of for

}

/**
 *  Phase#3 : Do a compaction for a single Region.
 * 						 This behavior is multiple thread safe.
 * 
 * Source : SemeruHeapRegion->_alive_bitmap
 * Dest	  : SemeruHeapRegion->_dest_region_ms + _dest_offset_ms
 * 
 * [?] How to handle the hugongous Regions separately ?
 * 		=> in phase#2, we set humongous Region's forwarding pointer to itself, which will not move any objects in them.
 * 
 */
void G1SemeruSTWCompactTask::phase3_compact_region(SemeruHeapRegion* hr) {

	log_debug(semeru, mem_compact)("%s, Enter Semeru MS Compact Phase#3, object compactation, worker [0x%x] ", __func__, this->_worker_id);

	assert(!hr->is_humongous(), "Should be no humongous regions in compaction queue");

	// Warning : for a void parameter constructor, do not assign () at the end.
  G1SemeruCompactRegionClosure semeru_ms_compact;			// the closure to evacuate a single alive object to dest
  hr->apply_to_marked_objects(hr->alive_bitmap(), &semeru_ms_compact);  // Do this compaction in the bitmap.

	//
	// [?]Check the compaction is not interrupped. How ?
	//
	
	//check_overflow_taskqueue("At the end of phase3, copy");




	// clear process.
	// 1) Restore Compaction,e.g. _compaction_top, information to normal fields, e.g. _top
  // 2) Clear not used range.
	hr->complete_compaction();
}



/**
 * Update the fields points to other region.
 * The region can be in current or other servers.
 * 
 * Warning. This is the old/original region, before compaction. 
 * But the field addr stored in SemeruHeapRegion->_inter_region_ref_queue is the new address.
 * 
 */
void G1SemeruSTWCompactTask::phase4_adjust_inter_region_pointer(SemeruHeapRegion* hr){

	log_debug(semeru, mem_compact)("%s, Enter Semeru MS Compact Phase#4, Inter-Region reference adjustment, worker [0x%x] ", __func__, this->_worker_id);

	// Check the item of G1SemeruSTWCompactTask->_inter_region_ref_queue
	check_overflow_taskqueue("phase4 Debug");
	

}






//
// Closures for G1SemeruSTWCompactTask -> Phase #1, preparation.
//


G1SemeruCalculatePointersClosure::G1SemeruCalculatePointersClosure( G1SemeruSTWCompact* semeru_sc,
																																		G1CMBitMap* bitmap,
                                                             				G1SemeruCompactionPoint* cp,
																																		uint* humongous_regions_removed) :
  _semeru_sc(semeru_sc),
  _bitmap(bitmap),
  _cp(cp),
	_humongous_regions_removed(humongous_regions_removed) { 

		#ifdef ASSERT
			tty->print("%s, initialized G1SemeruCalculatePointersClosure. ", __func__);
		#endif
}



/**
 * Tag : the FullGC threads is summarizing a source Region's destination Region.
 * 
 * [x] The claimed source Region will be added into G1FullGCPrepareTask->G1SemeruCompactionPoint
 *      which points to collector()->compaction_point(worker_id).
 * 
 */
bool G1SemeruCalculatePointersClosure::do_heap_region(SemeruHeapRegion* hr) {
  if (hr->is_humongous()) {     // 1) Humongous objects are not moved
    oop obj = oop(hr->humongous_start_region()->bottom());
    if (_bitmap->is_marked(obj)) {      // Both start and following humongous Regions should be marked
      if (hr->is_starts_humongous()) {  // Only process the start humongous Region.
        obj->forward_to(obj);  // not moving the humongous objects.
      }
    } else {
      free_humongous_region(hr);
    }
  } else if (!hr->is_pinned()) {
    prepare_for_compaction(hr);   // 2) Normal objects  
  }

  // Reset data structures not valid after Full GC.
  reset_region_metadata(hr);

  return false;
}



/**
 * Tag : a humongous Region need to be freed separately ?? 
 *  
 */
void G1SemeruCalculatePointersClosure::free_humongous_region(SemeruHeapRegion* hr) {

	//debug
	tty->print("%s, Error Not implement this function yet. \n", __func__);


  // FreeRegionList dummy_free_list("Dummy Free List for G1MarkSweep");

  // hr->set_containing_set(NULL);
  // _humongous_regions_removed++;

  // _g1h->free_humongous_region(hr, &dummy_free_list);
  // prepare_for_compaction(hr);   // Add this Region into Destination Region candidates
  // dummy_free_list.remove_all();
}





/**
 *  Handle the Remerber set infor mation.
 *  
 */
void G1SemeruCalculatePointersClosure::reset_region_metadata(SemeruHeapRegion* hr) {


	//debug
	tty->print("%s, Error Not implement this function yet. \n", __func__);

  // hr->rem_set()->clear();
  // hr->clear_cardtable();

  // if (_g1h->g1_hot_card_cache()->use_cache()) {
  //   _g1h->g1_hot_card_cache()->reset_card_counts(hr);
  // }
}




/**
 * Semeru MS : Add a Region into current compaction thread's CompactionPoint.
 *  
 */
void G1SemeruCalculatePointersClosure::prepare_for_compaction(SemeruHeapRegion* hr) {


	//debug
	tty->print("%s, Calculate destination for alive objects in Region[0x%lx] \n",__func__, (size_t)hr->hrm_index());

  if (!_cp->is_initialized()) {   	// if G1SemeruCompactionPoint is not setted, compact to itself.
    hr->set_compaction_top(hr->bottom());
    _cp->initialize(hr, true);			// Enqueue current Source Region as the first Destination Region.
  }
  // Enqueue this Region into destination Region queue.
  _cp->add(hr);
  prepare_for_compaction_work(_cp, hr);

}


/**
 * Tag : Calculate the destination for source Region's alive objects.
 *  
 * Parameter:
 *   cp : the destination Region. Multiple source Regions may be compacted to it.
 *   hr : The source Region, who should be compacted to cp.
 * 
 */
void G1SemeruCalculatePointersClosure::prepare_for_compaction_work(G1SemeruCompactionPoint* cp,
                                                                                  SemeruHeapRegion* hr) {
  G1SemeruPrepareCompactLiveClosure prepare_compact(cp);
  hr->set_compaction_top(hr->bottom());     // SemeruHeapRegion->_compaction_top is that if using this Region as a Destination, this is his top.
  hr->apply_to_marked_objects(_bitmap, &prepare_compact);
}


/**
 * If any Region is freed.
 *  
 */
bool G1SemeruCalculatePointersClosure::freed_regions() {
  if (_humongous_regions_removed > 0) {
    // Free regions from dead humongous regions.
    return true;
  }

  if (!_cp->has_regions()) {
    // No regions in queue, so no free ones either.
    return false;
  }

  if (_cp->current_region() != _cp->regions()->last()) {
    // The current region used for compaction is not the last in the
    // queue. That means there is at least one free region in the queue.
    return true;
  }

  // No free regions in the queue.
  return false;
}


// Live object closure
//

G1SemeruPrepareCompactLiveClosure::G1SemeruPrepareCompactLiveClosure(G1SemeruCompactionPoint* cp) :
    _cp(cp) { }


/**
 * Tag : Preparation Phase #2, calculate the destination for each alive object in the source/current Region. 
 *        G1SemeruCompactionPoint* _cp, stores the destination Regions.
 */
size_t G1SemeruPrepareCompactLiveClosure::apply(oop object) {
  size_t size = object->size();
  _cp->forward(object, size);
  return size;
}





//
// Closures for G1SemeruSTWCompactTask -> Phase #2, adjust pointer
//










//
// Phase#3, do the alive object copy.
//




/**
 * Evacuate an alive object to the destination.
 * 
 * [?] Guarantee the MT safe ?
 * 		Case 1)  pre-calculate the alive objects destination. 
 * 			e.g. add one more pass to store the destination address in each alive object's markoop.
 * 			[x] Semeru MS takes this design. pre-estimate the destination address for the alive objects.
 * 			And then each compact thread can do the copy parallelly.
 * 
 * 		Case 2) Add local cache for the Concurrent GC Threads.
 * 			Need to be synchronized when requesting the thread local cache from Region.
 * 
 * [x] In order to support interruption and save scanning time, don't use the forwarding pointer desgin.
 * 		 Put forwarding pointer in alive object's markOop is un-recoverable.
 * 		 Pick a suitable Region to be evacuated to current destination Region. 
 * 	
 * [?] Need to maintain a <src, dst> pair to record the object moving information for inter-region field update.
 * 
 */
size_t G1SemeruCompactRegionClosure::apply(oop obj) {
  size_t size = obj->size();
  HeapWord* destination = (HeapWord*)obj->forwardee();   // [?] already moved
  if (destination == NULL) {
    // Object not moving
    return size;
  }

  // copy object and reinit its mark
  HeapWord* obj_addr = (HeapWord*) obj;
  assert(obj_addr != destination, "everything in this pass should be moving");
  Copy::aligned_conjoint_words(obj_addr, destination, size);      // 4 bytes alignment copy ?
  oop(destination)->init_mark_raw();    // initialize the MarkOop.
  assert(oop(destination)->klass() != NULL, "should have a class");

	log_debug(semeru,mem_compact)("Phase4,copy obj from 0x%lx to 0x%lx ", (size_t)obj_addr, (size_t)destination );

  return size;
}








//
// Debug functions
//


void G1SemeruSTWCompact::print_stats() {
	if (!log_is_enabled(Debug, gc, stats)) {
		return;
	}
	log_debug(gc, stats)("---------------------------------------------------------------------");
	for (size_t i = 0; i < _num_active_tasks; ++i) {
		//	_tasks[i]->print_stats();  // Semeru MS doesn't have any code tasks

		tty->print("%s, Not implemented yet \n",__func__);

		log_debug(gc, stats)("---------------------------------------------------------------------");
	}
}



// Drain the both overflow queue and taskqueue
void G1SemeruSTWCompactTask::check_overflow_taskqueue( const char* message){
  StarTask ref;
  size_t count;
  SemeruCompactTaskQueue* inter_region_ref_queue = this->inter_region_ref_taskqueue();

  log_debug(semeru,mem_compact)("\n%s, start for Semeru MS CompactTask [0x%lx]", message, (size_t)worker_id() );


  // #1 Drain the overflow queue
  count =0;
	while (inter_region_ref_queue->pop_overflow(ref)) {
    oop const obj = RawAccess<>::oop_load((oop*)ref);
		if(obj!= NULL && (size_t)obj != (size_t)0xbaadbabebaadbabe){
			 log_debug(semeru,mem_compact)(" Overflow: ref[0x%lx] 0x%lx points to obj 0x%lx, klass 0x%lx, layout_helper 0x%x. is TypeArray ? %d",
										           count, (size_t)(oop*)ref ,(size_t)obj, (size_t)obj->klass(), obj->klass()->layout_helper(), obj->is_typeArray()  );

		}else{
       log_debug(semeru,mem_compact)(" Overflow: ERROR Find filed 0x%lx points to 0x%lx",(size_t)(oop*)ref, (size_t)obj);
    }

    count++;
  }


  // #1 Drain the task queue
  count =0;
  while (inter_region_ref_queue->pop_local(ref, 0 /*threshold*/)) { 
    oop const obj = RawAccess<>::oop_load((oop*)ref);
		if(obj!= NULL && (size_t)obj != (size_t)0xbaadbabebaadbabe){
		 log_debug(semeru,mem_compact)(" ref[0x%lx] 0x%lx points to obj 0x%lx, klass 0x%lx, layout_helper 0x%x. is TypeArray ? %d",
										           count, (size_t)(oop*)ref ,(size_t)obj, (size_t)obj->klass(), obj->klass()->layout_helper(), obj->is_typeArray()  );

		}else{
      log_debug(semeru,mem_compact)(" ERROR Find filed 0x%lx points to 0x%lx",(size_t)(oop*)ref, (size_t)obj );
    }

    count++;
  }



  assert(inter_region_ref_queue->is_empty(), "should drain the queue");

  log_debug(semeru,mem_compact)("%s, End for Semeru MS CompactTask [0x%lx] \n", message, (size_t)worker_id());
}

