/*
 * Copyright (c) 2001, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/classLoaderDataGraph.hpp"
#include "gc/g1/g1Analytics.hpp"
//#include "gc/g1/g1CollectedHeap.inline.hpp"
//#include "gc/g1/g1ConcurrentMark.inline.hpp"
//#include "gc/g1/g1ConcurrentMarkThread.inline.hpp"
#include "gc/g1/g1MMUTracker.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1VMOperations.hpp"
#include "gc/shared/concurrentGCPhaseManager.hpp"
#include "gc/shared/gcId.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/shared/gcTraceTime.inline.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/debug.hpp"


// Semeru
#include "gc/g1/g1SemeruConcurrentMark.inline.hpp"
#include "gc/g1/g1SemeruCollectedHeap.inline.hpp"
#include "gc/g1/g1SemeruConcurrentMarkThread.inline.hpp"


// ======= Semeru Concurrent Mark Thread ========
//  Semeru concurrent threads need to be controlled separately. 
//  And they execute specific CMTasks,
//  They have different phases 


// Check order in EXPAND_CURRENT_PHASES
STATIC_ASSERT(ConcurrentGCPhaseManager::UNCONSTRAINED_PHASE <
              ConcurrentGCPhaseManager::IDLE_PHASE);

/**
 * Tag : Purpose of each Phase
 * 
 * CLEAR_CLAIMED_MARKS : Clean the marked bit on prev/next_bitmap?
 * 
 * 
 */
#define EXPAND_CONCURRENT_PHASES(expander)                                 \
  expander(ANY, = ConcurrentGCPhaseManager::UNCONSTRAINED_PHASE, NULL)     \
  expander(IDLE, = ConcurrentGCPhaseManager::IDLE_PHASE, NULL)             \
  expander(SEMERU_CONCURRENT_CYCLE,, "Semeru Concurrent Cycle")            \
  expander(CLEAR_CLAIMED_MARKS,, "Concurrent Clear Claimed Marks")         \
  expander(SCAN_ROOT_REGIONS,, "(Abandoned in Semeru)Concurrent Scan Root Regions")  \
  expander(SEMERU_CONCURRENT_MARK,, "Semeru Concurrent Mark")              \
  expander(SEMERU_MEM_SERVER_CONCURRENT,, "Semeru Concurrent Mark")        \
  expander(MARK_FROM_ROOTS,, "(Abandoned in Semeru)Concurrent Mark From Roots")      \
  expander(PRECLEAN,, "Concurrent Preclean")                               \
  expander(BEFORE_REMARK,, NULL)                                           \
  expander(SEMERU_REMARK,, "Semeru STW Remark")                            \
  expander(SEMERU_COMPACT,, "Semeru STW Compact")                          \
  expander(REBUILD_REMEMBERED_SETS,, "Concurrent Rebuild Remembered Sets") \
  expander(CLEANUP_FOR_NEXT_MARK,, "Concurrent Cleanup for Next Mark")     \
  /* */


/**
 * Rewrite the concurrent phase
 *  
 */
class G1SemeruConcurrentPhase : public AllStatic {
public:
  enum {
#define CONCURRENT_PHASE_ENUM(tag, value, ignore_title) tag value,
    EXPAND_CONCURRENT_PHASES(CONCURRENT_PHASE_ENUM)
#undef CONCURRENT_PHASE_ENUM
    PHASE_ID_LIMIT
  };
};



/**
 * Semeru Concurrent Threads
 * => Execute all of the Semeru memory server concurrent marking, refinement and other concurrent tasks.  
 * 
 */
G1SemeruConcurrentMarkThread::G1SemeruConcurrentMarkThread(G1SemeruConcurrentMark* semeru_cm) :
  ConcurrentGCThread(),
  _vtime_start(0.0),
  _vtime_accum(0.0),
  _vtime_mark_accum(0.0),
  _semeru_cm(semeru_cm),           // use semeru marker
  _state(Idle),
  _phase_manager_stack() {

  set_name("Semeru CM Marker");
  create_and_start();     // [?] This will invoke the G1SemeruConcurrentThread->run_service()

  // debug
  //printf("ERROR in %s, Please fix me.\n", __func__);
}





/**
 * Semeru Memory Server
 * [?] But seems that we don't need to build a separate VM_Operation,
 *     It can be merged into Concurrent Marking task.
 *     Because there is no need to let the memory srever suspend the mutators.
 *     The CPU server already does this and inform the memory server its state.
 *     So the memory server only needs to switch its execution function.
 *  
 */
class CMRemark : public VoidClosure {
  G1SemeruConcurrentMark* _semeru_cm;
public:
  CMRemark(G1SemeruConcurrentMark* semeru_cm) : _semeru_cm(semeru_cm) {}

  void do_void(){
    _semeru_cm->remark();
  }
};


class CMCleanup : public VoidClosure {
  G1SemeruConcurrentMark* _semeru_cm;
public:
  CMCleanup(G1SemeruConcurrentMark* semeru_cm) : _semeru_cm(semeru_cm) {}

  void do_void(){
    _semeru_cm->cleanup();
  }
};

double G1SemeruConcurrentMarkThread::mmu_sleep_time(G1Policy* g1_policy, bool remark) {
  // There are 3 reasons to use SuspendibleThreadSetJoiner.
  // 1. To avoid concurrency problem.
  //    - G1MMUTracker::add_pause(), when_sec() and its variation(when_ms() etc..) can be called
  //      concurrently from ConcurrentMarkThread and VMThread.
  // 2. If currently a gc is running, but it has not yet updated the MMU,
  //    we will not forget to consider that pause in the MMU calculation.
  // 3. If currently a gc is running, ConcurrentMarkThread will wait it to be finished.
  //    And then sleep for predicted amount of time by delay_to_keep_mmu().
  SuspendibleThreadSetJoiner sts_join;

  const G1Analytics* analytics = g1_policy->analytics();
  double now = os::elapsedTime();
  double prediction_ms = remark ? analytics->predict_remark_time_ms()
                                : analytics->predict_cleanup_time_ms();
  G1MMUTracker *mmu_tracker = g1_policy->mmu_tracker();
  return mmu_tracker->when_ms(now, prediction_ms);
}

void G1SemeruConcurrentMarkThread::delay_to_keep_mmu(G1Policy* g1_policy, bool remark) {
  if (g1_policy->adaptive_young_list_length()) {
    jlong sleep_time_ms = mmu_sleep_time(g1_policy, remark);
    if (!_semeru_cm->has_aborted() && sleep_time_ms > 0) {
      os::sleep(this, sleep_time_ms, false);
    }
  }
}


/**
 * Semeru Memory Server - What's this timer used for ??
 * 
 * [?] Record the elapsed time for each phase ?
 *  
 */
class G1SemeruConcPhaseTimer : public GCTraceConcTimeImpl<LogLevel::Info, LOG_TAGS(gc, marking)> {
  G1SemeruConcurrentMark* _semeru_cm;

 public:
  G1SemeruConcPhaseTimer(G1SemeruConcurrentMark* semeru_cm, const char* title) :
    GCTraceConcTimeImpl<LogLevel::Info,  LogTag::_gc, LogTag::_marking>(title),
    _semeru_cm(semeru_cm)
  {
    _semeru_cm->gc_timer_cm()->register_gc_concurrent_start(title);
  }

  ~G1SemeruConcPhaseTimer() {
    _semeru_cm->gc_timer_cm()->register_gc_concurrent_end();
  }
};

static const char* const concurrent_phase_names[] = {
#define CONCURRENT_PHASE_NAME(tag, ignore_value, ignore_title) XSTR(tag),
  EXPAND_CONCURRENT_PHASES(CONCURRENT_PHASE_NAME)
#undef CONCURRENT_PHASE_NAME
  NULL                          // terminator
};
// Verify dense enum assumption.  +1 for terminator.
STATIC_ASSERT(G1SemeruConcurrentPhase::PHASE_ID_LIMIT + 1 ==
              ARRAY_SIZE(concurrent_phase_names));

// Returns the phase number for name, or a negative value if unknown.
static int lookup_concurrent_phase(const char* name) {
  const char* const* names = concurrent_phase_names;
  for (uint i = 0; names[i] != NULL; ++i) {
    if (strcmp(name, names[i]) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// The phase must be valid and must have a title.
static const char* lookup_concurrent_phase_title(int phase) {
  static const char* const titles[] = {
#define CONCURRENT_PHASE_TITLE(ignore_tag, ignore_value, title) title,
    EXPAND_CONCURRENT_PHASES(CONCURRENT_PHASE_TITLE)
#undef CONCURRENT_PHASE_TITLE
  };
  // Verify dense enum assumption.
  STATIC_ASSERT(G1SemeruConcurrentPhase::PHASE_ID_LIMIT == ARRAY_SIZE(titles));

  assert(0 <= phase, "precondition");
  assert((uint)phase < ARRAY_SIZE(titles), "precondition");
  const char* title = titles[phase];
  assert(title != NULL, "precondition");
  return title;
}

class G1SemeruConcPhaseManager : public StackObj {
  G1SemeruConcurrentMark* _semeru_cm;   // Points to the passed one.
  ConcurrentGCPhaseManager _manager;    // Create a new ConcurrentGC Phase Manager.

public:
  G1SemeruConcPhaseManager(int phase, G1SemeruConcurrentMarkThread* thread) :
    _semeru_cm(thread->semeru_cm()),
    _manager(phase, thread->phase_manager_stack())
  { }

  ~G1SemeruConcPhaseManager() {
    // Deactivate the manager if marking aborted, to avoid blocking on
    // phase exit when the phase has been requested.
    if (_semeru_cm->has_aborted()) {
      _manager.deactivate();
    }
  }

  void set_phase(int phase, bool force) {
    _manager.set_phase(phase, force);
  }
};




/** 
 * Semeru Memory Server - Combine phase management and timing into one convenient utility.
 * 
 * [?] Maintain separate phase manager and timer for the Semeru Memory Server GC。
 * 
 */ 
class G1SemeruConcPhase : public StackObj {
  G1SemeruConcPhaseTimer _timer;
  G1SemeruConcPhaseManager _manager;

public:
  G1SemeruConcPhase(int phase, G1SemeruConcurrentMarkThread* thread) :
    _timer(thread->semeru_cm(), lookup_concurrent_phase_title(phase)),
    _manager(phase, thread)
  { }
};

const char* const* G1SemeruConcurrentMarkThread::concurrent_phases() const {
  return concurrent_phase_names;
}

bool G1SemeruConcurrentMarkThread::request_concurrent_phase(const char* phase_name) {
  int phase = lookup_concurrent_phase(phase_name);
  if (phase < 0) return false;

  while (!ConcurrentGCPhaseManager::wait_for_phase(phase,
                                                   phase_manager_stack())) {
    assert(phase != G1SemeruConcurrentPhase::ANY, "Wait for ANY phase must succeed");
    if ((phase != G1SemeruConcurrentPhase::IDLE) && !during_cycle()) {
      // If idle and the goal is !idle, start a collection.
      G1CollectedHeap::heap()->collect(GCCause::_wb_conc_mark);
    }
  }
  return true;
}


/**
 * Semeru Memory Server
 *  
 *  Run the Semeru concurrent marking, STW Remark , STW compaction in background. 
 *  [?] Which thread is executing this service ?
 *    => a special concurrent thread
 * 
 *  [?] This service keeps running in background ?
 * 
 *  [?] Is this a deamon thread, running in the background ??
 *        => So it needs to check if new Regions are assigned to it.
 *  
 *  [?] In the original design, we need to switch the STW, Concurrent mode for different phases.  
 *      But for Semeru, we don't have such a requirements. 
 *      After the CPU server informs Memory server, it's in GC or not,
 *      memory server can switch between different work sharply by just switching a function.
 * 
 */
void G1SemeruConcurrentMarkThread::run_service() {
  _vtime_start = os::elapsedVTime();

  G1SemeruCollectedHeap* semeru_heap = G1SemeruCollectedHeap::heap();
  G1Policy* g1_policy = semeru_heap->g1_policy();

  //
  //  [?] Check if we have received any assigned Regions. 
  //      Collection Set for the Memory Server. It's only used to record Region Index.
  //      The concurrent scavenge is applied to each Region one by one.
  //      
  // debug
  //size_t region_index_to_scan = 0; // e.g. Scan the HeapRegion[0].

  // Semeru phase manager
  G1SemeruConcPhaseManager cpmanager(G1SemeruConcurrentPhase::IDLE, this);      // [?] rewrite the phase manager for semeru


  // [x] The Semeru Memory Concurrent Marking service is started,
  //     Let the Semeru Concurrent threads, current thread, wait on SemeruCGC_lock.
  //     These Semeru Concurrent threads will be waken up by the RDMA driver via CPU server. 
  while (!should_terminate()) {
    // wait until started is set.
    sleep_before_next_cycle();    // [XX]Concurrent thread is waiting for the RDMA signal from CPU server.
    if (should_terminate()) {
      break;
    }

    // [?] What's  the purpose of these phase ?
    //    Just for Log ? Can also synchronize some thing?
    //     If this is the case, we have to sperate Semeru Concurrent and the normal concurrent.
    //    The most simplest way is to disable the traditional Concurrent Thread build and schedule.
    cpmanager.set_phase(G1SemeruConcurrentPhase::SEMERU_CONCURRENT_CYCLE, false /* force */);

    GCIdMark gc_id_mark;

    // the _semeru_cm field isn't NULL. g1ConcurrentMarkThread->_semeru_cm is initialized and used by g1CollectedHeap.
    // But never use the _semeru_cm field in current semeru service.
    //
    // [?] What operations have been done before GC ??
    //
    _semeru_cm->concurrent_cycle_start();     // tag logs 

    GCTraceConcTime(Info, gc) tt("Semeru Concurrent Cycle");
    {
      ResourceMark rm;
      HandleMark   hm;
      double cycle_start = os::elapsedVTime();

      {
        // 1) Dispatch the received CSets from Memory Server
        // Check if we received new CSet from CPU server.
        // CPU server keeps pushing new CSet to Memory server.
        // [X] This procedure has to by the main thread. This is NOT MT safe.
        //
        //  The CSet is transferred from CPU Server,
        //  G1SemeruConcurrentMark->_finger points to the start of the memory server CSet.
        //  the scanning sequence is controlled by G1SemeruCMTask->_curr_region.
        //  
        //  a. Divide the received Regions to 2 sets, Scanned, Freshly evicted.
        //  b. Apply STW Remark to the Scanned Regions, and try to compact them during the STW window.
        //  c. And then do concurrent marking for the Freshly evicted Regions.


        #ifdef ASSERT
        if(Thread::current() != NULL && Thread::current()->is_Named_thread()){
          tty->print("%s, Named Thread %s, id[%u] is running here. \n", __func__, 
                                                ((NamedThread*)Thread::current())->name(), 
                                                ((NamedThread*)Thread::current())->gc_id() );
        }else{
          tty->print("%s, Unknown thread [0x%lx] is running here. \n",__func__, (size_t)Thread::current());
        }
        #endif

        received_memory_server_cset* recv_mem_server_cset = semeru_heap->recv_mem_server_cset();

        // Enqueue the received Regions to _cm_scanned_region, _freshly_evicetd_regions.
        dispatch_received_regions(recv_mem_server_cset);

      }


      // {
      //   //
      //   // [?] What's this for ?
      //   //     
      //   // [?] What's the conenction with ClassLoader ?
      //   // [?] For Semeru, does it need to process the CLD for semeru memory pool ?? 
      //   // 
      //   G1SemeruConcPhase p(G1SemeruConcurrentPhase::CLEAR_CLAIMED_MARKS, this);
      //   ClassLoaderDataGraph::clear_claimed_marks();
      // }

      // 2) Utilize the STW window to compact the alrady scanned Regions in CSet.
      //    Try to do Remark and Compact first every time a CPU Server GC send a signal here ??
      //    [?] How to implement this ??
      //    ==> Seems that we don't need a sperate PHASE for the Remark and Compact.
      //        After receiving the signal from CPU server, switch to cliam a cm_scanned Region and Remark-Compact it.
      //
      // {
      // //  If the Regions are already traces, do the Remark in STW.
      // //
      // //    [?] Because the Remark is STW, so here  have to build a STW operation ?
      // //        For the concurrent operation, e.g. Semeru CM GC phase, just implement it as a function
      // //        and invoke it in current function ?



      //     // Pause Remark.
      //     // [?] How to pause the mutators ??
      //     //  => The mutators should be paused by GC Task the scheduler 
      //     log_info(gc, marking)("%s (%.3fs, %.3fs) %.3fms",
      //                           cm_title,
      //                           TimeHelper::counter_to_seconds(mark_start),
      //                           TimeHelper::counter_to_seconds(mark_end),
      //                           TimeHelper::counter_to_millis(mark_end - mark_start));

      //     mark_manager.set_phase(G1SemeruConcurrentPhase::REMARK, false);   // [?] purpose of this action ?
      //     CMRemark cl(_semeru_cm);
      //     VM_G1Concurrent op(&cl, "Pause Remark and Compact");
      //     VMThread::execute(&op);     // Suspend the mutators here ?? This is required by the VM_G1Concurrent ??
      //     if (_semeru_cm->has_aborted()) {
      //       break;
      //     } else if (!_semeru_cm->restart_for_overflow()) {
            
      //       // [?] What's the logic here ? if task_queue is overflow, restart the CM totally ???

      //       break;              // Exit loop if no restart requested.
      //     } else {
      //       // Loop to restart for overflow.
      //       mark_manager.set_phase(G1SemeruConcurrentPhase::SEMERU_CONCURRENT_MARK, false);
      //       log_info(gc, marking)("%s Restart for Mark Stack Overflow (iteration #%u)",
      //                             cm_title, iter);

      //     }

      // } // End of Remark and Compact phase, STW 
  
      //
      // Abandon this phase.
      //
      // We have to ensure that we finish scanning the root regions
      // before the next GC takes place. To ensure this we have to
      // make sure that we do not join the STS until the root regions
      // have been scanned. If we did then it's possible that a
      // subsequent GC could block us from joining the STS and proceed
      // without the root regions have been scanned which would be a
      // correctness issue.

      // {
      //   G1SemeruConcPhase p(G1SemeruConcurrentPhase::SCAN_ROOT_REGIONS, this);
      //   _semeru_cm->scan_root_regions();
      // }


      //
      // 3) Concurrent Phase :  Concurrently trace the fresh evicted Regions.
      //
      // It would be nice to use the G1SemeruConcPhase class here but
      // the "end" logging is inside the loop and not at the end of
      // a scope. Also, the timer doesn't support nesting.
      // Mimicking the same log output instead.
      
      {

        // [?] Why does it need a second phase manager ?? There is already a cpmanager ?
        //
        //G1SemeruConcPhaseManager mark_manager(G1SemeruConcurrentPhase::SEMERU_CONCURRENT_MARK, this);
        jlong mark_start = os::elapsed_counter();
        const char* cm_title = lookup_concurrent_phase_title(G1SemeruConcurrentPhase::SEMERU_CONCURRENT_MARK);
        log_info(gc, marking)("%s (%.3fs)",
                              cm_title,
                              TimeHelper::counter_to_seconds(mark_start));

        // [?] A dead loop, is this correct ??
        //  Or we should also put the Remark && Compact within this loop ?
        for (uint iter = 1; !_semeru_cm->has_aborted(); ++iter) {
      //     // Concurrent marking.
      //     //
      //     // [?] Difference with Mark from Root Regions ?
      //     //      Marking from roots, mark from thread variables ??
      //     {
      //       G1SemeruConcPhase p(G1SemeruConcurrentPhase::MARK_FROM_ROOTS, this);
      //       _semeru_cm->mark_from_roots();    // The main content of Concurrent Marking 
      //     }

          // Change the name to CM ?
          // We reuse the name,MARK_FROM_ROOTS, temporarily 
          {
            G1SemeruConcPhase p(G1SemeruConcurrentPhase::SEMERU_MEM_SERVER_CONCURRENT, this);
            _semeru_cm->semeru_concurrent_marking();    // The main content of Concurrent Marking 
          }

          // Do we need to check the aborted so frequently ?
          if (_semeru_cm->has_aborted()) {
            break;
          }

          // [?] What's the purpose of pre-cleaning ? do we need it ?
      //     if (G1UseReferencePrecleaning) {
      //       G1SemeruConcPhase p(G1SemeruConcurrentPhase::PRECLEAN, this);
      //       _semeru_cm->preclean();
      //     }

      //     // Provide a control point before remark.
      //     {
      //       G1SemeruConcPhaseManager p(G1SemeruConcurrentPhase::BEFORE_REMARK, this);
      //     }
      //     if (_semeru_cm->has_aborted()) {
      //       break;
      //     }

      //     // Delay remark pause for MMU.
      //     double mark_end_time = os::elapsedVTime();
      //     jlong mark_end = os::elapsed_counter();
      //     _vtime_mark_accum += (mark_end_time - cycle_start);
      //     delay_to_keep_mmu(g1_policy, true /* remark */);
      //     if (_semeru_cm->has_aborted()) {
      //       break;
      //     }





        } // The for loop of concurrent marking(tracing).
      
      } // End of Concurrent tracing phases 



      //
      // 4) After finish the incremental Remarking, compact the Region.
      //

      // Abandon the remember set rebuild process.
      // if (!_semeru_cm->has_aborted()) {
      //   G1SemeruConcPhase p(G1SemeruConcurrentPhase::REBUILD_REMEMBERED_SETS, this);
      //   _semeru_cm->rebuild_rem_set_concurrently();
      // }

      double end_time = os::elapsedVTime();
      // Update the total virtual time before doing this, since it will try
      // to measure it to get the vtime for this marking.
      _vtime_accum = (end_time - _vtime_start);

      //
      // [?] What's the reason for this *mmu* related things.
      //
      // if (!_semeru_cm->has_aborted()) {
      //   delay_to_keep_mmu(g1_policy, false /* cleanup */);
      // }


      // CLeanup phase, Change THIS to STW compaction ?
      // if (!_semeru_cm->has_aborted()) {
      //   CMCleanup cl_cl(_semeru_cm);
      //   VM_G1Concurrent op(&cl_cl, "Pause Cleanup");
      //   VMThread::execute(&op);
      // }

      // We now want to allow clearing of the marking bitmap to be
      // suspended by a collection pause.
      // We may have aborted just before the remark. Do not bother clearing the
      // bitmap then, as it has been done during mark abort.
      // if (!_semeru_cm->has_aborted()) {
      //   G1SemeruConcPhase p(G1SemeruConcurrentPhase::CLEANUP_FOR_NEXT_MARK, this);
      //   _semeru_cm->cleanup_for_next_mark();
      // }

    }  // End of all the concurrent  Cycles


    // Update the number of full collections that have been
    // completed. This will also notify the FullGCCount_lock in case a
    // Java thread is waiting for a full GC to happen (e.g., it
    // called System.gc() with +ExplicitGCInvokesConcurrent).
    {
      SuspendibleThreadSetJoiner sts_join;
     // semeru_heap->increment_old_marking_cycles_completed(true /* concurrent */);

      _semeru_cm->concurrent_cycle_end();
    }

    cpmanager.set_phase(G1SemeruConcurrentPhase::IDLE, _semeru_cm->has_aborted() /* force */);
  } // End of the while loop


  // [?] Is this correct to cancel all these regions ?

  //_semeru_cm->root_regions()->cancel_scan();
  _semeru_cm->mem_server_cset()->cancel_compact();
  _semeru_cm->mem_server_cset()->cancel_scan();

  // Debug
  // Suspend this thread agian.
  // Suspend in G1SemeruConcurrentMark->do_marking_step()
  //
  //set_idle();
  //sleep_before_next_cycle();
  //
  //

}


/**
 * Semeru Memory Server - Dispatch the received Regions CSet into scanned/freshly_evicted queues.
 * 
 * Warning : 
 *  1) this function isn't MT safe. 
 *  2) May add one Region multiple times.
 *      => It should be OK. because the first scavenge will process the region's Target_obj_queue.
 */
void G1SemeruConcurrentMarkThread::dispatch_received_regions(received_memory_server_cset* recv_mem_server_cset){

  assert(recv_mem_server_cset!= NULL, "%s, must initiate the G1SemeruCollectHeap->_mem_server_cset \n", __func__);

  G1SemeruCollectedHeap* semeru_heap = G1SemeruCollectedHeap::heap();
  //size_t* received_num = mem_server_cset->num_received_regions();
  int received_region_ind = recv_mem_server_cset->pop();  // can be negative 
   HeapRegion* region_received = NULL;

  while(received_region_ind != -1){
    region_received = semeru_heap->hrm()->at(received_region_ind);
    assert(region_received != NULL, "%s, received Region is invalid.", __func__);   // [?] how to confirm if this region is available ?

    if(region_received->is_region_cm_scanned()){
      // add region into _cm_scanned_regions[] queue
      // The add operation may add one Region mutiple times. 
      semeru_cm()->mem_server_cset()->add_cm_scanned_regions(region_received);
    }else{
      // add region into _freshly_evicted_regions[] queue
      semeru_cm()->mem_server_cset()->add_freshly_evicted_regions(region_received);
    }

  }// Received CSet isn't emtpy.


}





/**
 * Semeru Memory Server - Stop the running concurrent service ?
 *  => [?] Why do we need a Mutex to stop the concurrent service ?? 
 *    
 *    [?] If the purpose is to stop the concurrent service, 
 *        why do the concurrent threads need to acuire the SemeruCGC_lock ?
 *        
 * 
 * 
 */
void G1SemeruConcurrentMarkThread::stop_service() {
  MutexLockerEx ml(SemeruCGC_lock, Mutex::_no_safepoint_check_flag);
  SemeruCGC_lock->notify_all();   // [?] notify which threads ??
}


/**
 * Suspend all the concurrent threads, waiting on SemeruCGC_lock.
 *  
 */
void G1SemeruConcurrentMarkThread::sleep_before_next_cycle() {
  // We join here because we don't want to do the "shouldConcurrentMark()"
  // below while the world is otherwise stopped.
  assert(!in_progress(), "should have been cleared");

  MutexLockerEx x(SemeruCGC_lock, Mutex::_no_safepoint_check_flag);
  while (!started() && !should_terminate()) {       // [?] Why does here use a while() look ?
    SemeruCGC_lock->wait(Mutex::_no_safepoint_check_flag);    // If the threads already wait here, no need to use a while loop?
  }

  if (started()) {
    set_in_progress();
  }
}

/**
 * Debug function : Let current concurent thread wait on SemeruCGC_lock.
 * 
 * [?] the problem is that, the mutator also suspend here ? WHY ?
 * The VM Thread is also suspend some where ??
 *  
 */
void G1SemeruConcurrentMarkThread::sleep_on_semerucgc_lock(){
  set_idle();

  assert(!in_progress(), "should have been cleared");

  MutexLockerEx x(SemeruCGC_lock, Mutex::_no_safepoint_check_flag);
  while (!started() && !should_terminate()) {       // [?] Why does here use a while() look ?
    SemeruCGC_lock->wait(Mutex::_no_safepoint_check_flag);    // If the threads already wait here, no need to use a while loop?
  }

  if (started()) {
    set_in_progress();
  }

}
