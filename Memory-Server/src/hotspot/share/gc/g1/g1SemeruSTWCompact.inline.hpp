
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

	// if this obj is within current scanning Region.
	if( !curr_region->is_in(obj)){
		// Path #2, Inter-Region reference. Record and adjust it  in Phase#4.
		log_debug(semeru, compact)("%s, Record an inter-Region reference field 0x%lx --> obj 0x%lx", __func__,(size_t)p, (size_t)obj );

		// if the target object is in another Region, we don't to handle it.
		// When a Region is compacted, we send the new address of the Target Oop to its source to update them.
		//  --- incoming inter-Region ref ---> current Region --- outgoing inter-region ref-->
		#ifdef ASSERT
		
		tty->print("%s, mark the objects dirty in _inter_region_ref_bitmap. Not implemented. \n",__func__);
		//_curr_region->_inter_region_ref_bitmap.



		#endif

		return;
	}
		// Path#1, Intra-Region reference. Adjust it here.


  assert(Universe::heap()->is_in(obj), "should be in heap");
	assert(curr_region->is_in(obj), "should be in current compacting Region.");

  if (G1ArchiveAllocator::is_archived_object(obj)) {
    // We never forward archive objects.
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
  assert(Universe::heap()->is_in_reserved(forwardee), "should be in object space");
  RawAccess<IS_NOT_NULL>::oop_store(p, forwardee);
}



inline void G1SemeruAdjustClosure::do_oop(oop* p)       { do_oop_work(p); }
inline void G1SemeruAdjustClosure::do_oop(narrowOop* p) { do_oop_work(p); }





#endif