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
#include "gc/g1/g1Analytics.hpp"
//#include "gc/g1/g1CollectorPolicy.hpp"
#include "gc/g1/g1YoungGenSizer.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "gc/shared/gcPolicyCounters.hpp"
#include "runtime/globals.hpp"
#include "utilities/debug.hpp"

// Semeru
#include "gc/g1/g1SemeruCollectorPolicy.hpp"

/**
 * [?]We need to rewrite these  control parameters for Semeru
 *  
 */
G1SemeruCollectorPolicy::G1SemeruCollectorPolicy():
_semeru_max_heap_byte_size(SemeruMemPoolMaxSize),
_semeru_initial_heap_byte_size(SemeruMemPoolInitialSize),
_semeru_memory_pool_alignment(0) {    // Initialize the alignment size in initialize_alignments() 

  // Set up the region size and associated fields. Given that the
  // policy is created before the heap, we have to set this up here,
  // so it's done as soon as possible.

  // It would have been natural to pass initial_heap_byte_size() and
  // max_heap_byte_size() to setup_heap_region_size() but those have
  // not been set up at this point since they should be aligned with
  // the region size. So, there is a circular dependency here. We base
  // the region size on the heap size, but the heap size should be
  // aligned with the region size. To get around this we use the
  // unaligned values for the heap.
  HeapRegion::setup_semeru_heap_region_size(SemeruMemPoolInitialSize, SemeruMemPoolMaxSize);
  HeapRegionRemSet::setup_semeru_remset_size();

}

void G1SemeruCollectorPolicy::initialize_alignments() {

  // Some parameters for original heap  control.
  // If hese parameters are not static, can we just rewrite their value to Semeru paratmers. 
  _space_alignment = HeapRegion::GrainBytes;
  size_t card_table_alignment = CardTableRS::ct_max_alignment_constraint();
  size_t page_size = UseLargePages ? os::large_page_size() : os::vm_page_size();
  _heap_alignment = MAX3(card_table_alignment, _space_alignment, page_size);

  // Semeru patarmeters 
  _semeru_memory_pool_alignment = MAX3(card_table_alignment, HeapRegion::SemeruGrainBytes, page_size);
  log_info(heap)("%s, _semeru_memory_pool_alignment is 0x%llx ", __func__, 
                              (unsigned long long)_semeru_memory_pool_alignment);

}


bool G1SemeruCollectorPolicy::is_hetero_heap() const {
  return false;
}



