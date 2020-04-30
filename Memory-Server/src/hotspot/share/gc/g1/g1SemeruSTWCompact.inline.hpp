
#ifndef SHARE_VM_GC_G1_SEMERU_G1STWCOMPACT_INLINE_HPP
#define SHARE_VM_GC_G1_SEMERU_G1STWCOMPACT_INLINE_HPP

#include "gc/g1/g1SemeruSTWCompact.hpp"
#include "gc/g1/g1Allocator.inline.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"


// inline 
#include "gc/g1/SemeruHeapRegion.inline.hpp"
#include "gc/g1/g1SemeruCollectedHeap.inline.hpp"



//
// Phase#2, Pointer adjustment functions
//




/**
 * Tag: apply this pointer adjustment to each field of the alive objects.
 *      This scavenge is in the order of alive bitmap.
 *  
 * [x] This function only update the Intra-Region references.
 *     Inter-Region references will be recorded for Phase#4.
 * 
 */
template <class T> 
inline void G1SemeruAdjustClosure::adjust_intra_region_pointer(T* p, SemeruHeapRegion* curr_region) {
  T heap_oop = RawAccess<>::oop_load(p);
  if (CompressedOops::is_null(heap_oop)) {
    return;
  }

  oop obj = CompressedOops::decode_not_null(heap_oop);

	// There are 2 pathes for field update, based on if this obj is within current scanning Region.
  //
	if( !curr_region->is_in(obj)){
		// Path #2, Inter-Region reference. Record and adjust it  in Phase#4.
    // Even the target object is in a Region on current Memory Server's CSet.
    // We also need to use its SemeruHeapRegion->_cross_region_ref_update_queue to update this fields.
    // Because we don't know if the refererenced target Region is already compacted, whose forwarding pointer can be overrided, 
    // or that Region will not be compacted at all for this STW window.

		// We use a bitmap,_inter_region_ref_bitmap, to record the fields points to object in another Region.
    // The _inter_region_ref_bitmap is only used in Memory server. No need to allocate into the RDMA Meta space.
    // It's also safe to delete after the compaction.
		#ifdef ASSERT
		
		log_debug(semeru, mem_compact)("%s, Record an inter-Region reference field 0x%lx --> obj 0x%lx", __func__,(size_t)p, (size_t)obj );
		//_curr_region->_inter_region_ref_bitmap.

    // Push the &field, oop*, of p into queue.
    // Attention, use the new address of the field. The address after compaction.
    StarTask new_field_addr;

    //_inter_region_ref_queue->push()


		#endif

		return;
	}
		// Path#1, Intra-Region reference. Adjust it here.


  assert(Universe::semeru_heap()->is_in(obj), "should be in heap");
	assert(curr_region->is_in(obj), "should be in current compacting Region.");

  if (G1ArchiveAllocator::is_archived_object(obj)) {
    // We never forward archive objects.
    log_debug(semeru,mem_compact)("%s,target obj 0x%lx is in archived Region, can't be moved. skip pointer adjustment p 0x%lx -> it.", __func__,
                                   (size_t)obj, (size_t)p );
    return;
  }

  oop forwardee = obj->forwardee(); // get the new address of the referenced object.
  if (forwardee == NULL) {
    // Not forwarded, return current reference.
    assert(obj->mark_raw() == markOopDesc::prototype_for_object(obj) || // Correct mark
           obj->mark_raw()->must_be_preserved(obj) || // Will be restored by PreservedMarksSet
           (UseBiasedLocking && obj->has_bias_pattern_raw()), // Will be restored by BiasedLocking
           "Must have correct prototype or be preserved, obj: " PTR_FORMAT ", mark: " PTR_FORMAT ", prototype: " PTR_FORMAT,
           p2i(obj), p2i(obj->mark_raw()), p2i(markOopDesc::prototype_for_object(obj)));
    return;
  }

  // Forwarded, just update.
  assert(Universe::semeru_heap()->is_in_semeru_reserved(forwardee), "should be in object space");
  RawAccess<IS_NOT_NULL>::oop_store(p, forwardee);
}



inline void G1SemeruAdjustClosure::do_oop(oop* p)       { do_oop_work(p); }
inline void G1SemeruAdjustClosure::do_oop(narrowOop* p) { do_oop_work(p); }

inline void G1SemeruAdjustClosure::semeru_ms_do_oop(oop obj, oop* p) { do_oop_work(p); }
inline void G1SemeruAdjustClosure::semeru_ms_do_oop(oop obj, narrowOop* p) { do_oop_work(p); }





#endif