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

#ifndef SHARE_VM_GC_G1_G1CONCURRENTMARKTHREAD_HPP
#define SHARE_VM_GC_G1_G1CONCURRENTMARKTHREAD_HPP

#include "gc/shared/concurrentGCPhaseManager.hpp"
#include "gc/shared/concurrentGCThread.hpp"

// Semeru
#include "gc/shared/rdmaStructure.hpp"

class G1ConcurrentMark;
class G1Policy;

// Semeru
class G1SemeruConcurrentMark;


/**
 * The concurrent mark thread triggers the various steps of the concurrent marking
 * cycle, including various marking cleanup.
 * 
 * Tag 
 * 
 * [x] How many concurrent threads is running ?
 *  => controlled by the option -XX:ConcGCThreads=n
 * 
 * [?] How many concurrent threads for each region ?
 * 
 * 
 * [?] G1ConcurrentMarkThread -> ConcurrentGCThread 
 *  => This thread is only designed for concurrent marking ?
 *     Can the Semeru use it for compaction at the same time?
 * 
 * 
 * 
 */
class G1ConcurrentMarkThread: public ConcurrentGCThread {
  friend class VMStructs;

  double _vtime_start;  // Initial virtual time.
  double _vtime_accum;  // Accumulated virtual time.
  double _vtime_mark_accum;

  G1ConcurrentMark* _cm;

  // Semeru memory server marking & compaction ?
  G1SemeruConcurrentMark* _semeru_cm;   // only initialize this for semeru memory pool.

  enum State {
    Idle,
    Started,
    InProgress
  };

  volatile State _state;

  // WhiteBox testing support.
  ConcurrentGCPhaseManager::Stack _phase_manager_stack;

  void sleep_before_next_cycle();
  // Delay marking to meet MMU.
  void delay_to_keep_mmu(G1Policy* g1_policy, bool remark);
  double mmu_sleep_time(G1Policy* g1_policy, bool remark);

  void run_service();
  void stop_service();

  // semeru
 // void run_semeru_service();
 // void stop_semeru_service();

 public:
  // Constructor
  G1ConcurrentMarkThread(G1ConcurrentMark* cm);

  // Semeru
  G1ConcurrentMarkThread(G1SemeruConcurrentMark* cm);

  // Total virtual time so far for this thread and concurrent marking tasks.
  double vtime_accum();
  // Marking virtual time so far this thread and concurrent marking tasks.
  double vtime_mark_accum();

  G1ConcurrentMark* cm()   { return _cm; }

  // Semeru
  G1SemeruConcurrentMark* semeru_cm()   { return _semeru_cm; }

  void set_idle()          { assert(_state != Started, "must not be starting a new cycle"); _state = Idle; }
  bool idle()              { return _state == Idle; }
  void set_started()       { assert(_state == Idle, "cycle in progress"); _state = Started; }
  bool started()           { return _state == Started; }
  void set_in_progress()   { assert(_state == Started, "must be starting a cycle"); _state = InProgress; }
  bool in_progress()       { return _state == InProgress; }

  // Returns true from the moment a marking cycle is
  // initiated (during the initial-mark pause when started() is set)
  // to the moment when the cycle completes (just after the next
  // marking bitmap has been cleared and in_progress() is
  // cleared). While during_cycle() is true we will not start another cycle
  // so that cycles do not overlap. We cannot use just in_progress()
  // as the CM thread might take some time to wake up before noticing
  // that started() is set and set in_progress().
  bool during_cycle()      { return !idle(); }

  // WhiteBox testing support.
  const char* const* concurrent_phases() const;
  bool request_concurrent_phase(const char* phase);

  ConcurrentGCPhaseManager::Stack* phase_manager_stack() {
    return &_phase_manager_stack;
  }

};

#endif // SHARE_VM_GC_G1_G1CONCURRENTMARKTHREAD_HPP
