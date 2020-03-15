/**
 * This file contains all the class or structure used by RDMA transferring.
 *  
 */


#ifndef SHARE_GC_SHARED_RDMA_STRUCTURE
#define SHARE_GC_SHARED_RDMA_STRUCTURE

#include "utilities/align.hpp"
#include "utilities/sizes.hpp"
#include "utilities/globalDefinitions.hpp"
#include "memory/allocation.inline.hpp"
#include "gc/shared/taskqueue.inline.hpp"



#define RDMA_STRUCTURE_ALIGNMENT			16

#define MEM_SERVER_CSET_BUFFER_SIZE		(size_t)(512 - 8) 	


/**
 * CHeapRDMAObj allocation type.
 * This is used for allocating queue.
 * Decide its start address based on the queue type and corresponding value defined in globalDefinitions.hpp
 *  
 */
enum CHeapAllocType {
  CPU_TO_MEM_AT_INIT_ALLOCTYPE,   // 0,
  CPU_TO_MEM_AT_GC_ALLOCTYPE,     // 1,
  MEM_TO_CPU_AT_GC_ALLOCTYPE,     // 2,
  ALLOC_TARGET_OBJ_QUEUE,         // 3,
  NON_ALLOC_TYPE     // non type
};

/**
 * This allocator is used to allocate objects into fixed address for RDMA communications between CPU and memory server.
 * 
 * Template Parameter:
 *    class E : Used to calculate class array's element size.
 *    CHeapAllocType Alloc_type : Used to choose which RDMA meta space is allocated into.
 *  
 * 
 * [?] CHeapObj allocates object into C-Heap by malloc().
 *     We commit space from reserved space.
 *    
 * [x] We only need to use the operator new to invoke class's constructor.
 * 
 * [x] The RDMA class should use flexible array to store data.
 * 
 * [x] Its subclass may have non-static fields, so do NOT inherit from AllStatic class.
 * 
 */
template <class E , CHeapAllocType Alloc_type = NON_ALLOC_TYPE> 
class CHeapRDMAObj{
  friend class MmapArrayAllocator<E>;

private:

 


public:

  //
  //  Variables
  //

  // Used to calculate the offset for different instan's offset.
  // [xx] Each instance of this template class has its own copy of member static variables.
  //      So, based on different CHeapAllocType value, _alloc_ptr should be initiazlied to different value.
  //
  // [??] This filed is useless here for now.
  static char   *_alloc_ptr; 
  static size_t  _instance_size;  // only used for normal instance allocation. e.g. the obj in SemeruHeapRegion.


  //
  //  Functions
  //

  //
  // Warning : this funtion is invoked latter than operator new.
  CHeapRDMAObj(){

  }


  /**
   * new object instance #1
   * Allocate space for non-array object instance.
   *  1) Single structure.
   *  2) Content are stored in a flexible array
   *  3) Fixed start address.
   * 
   * 
   * More explanation
   * 
   * 1) this function is only used to commit space on reserved space.
   * 2) the new operation first, invoke this override function to allocate space
   *    second, it invokes the constructor to do initialization.
   *    BUT the return value of operator new, has to be void*.
   * 3) The override operator always work like static, 
   *     It can only invoke static functions.
   * 4) The first parameter, size, is assigned by Operator new. It's the size of the class.
   *    The commit size is passed by the caller,  
   *      a. to reserve space for the flexible array at the end of the class
   *      b. for alignment.
	 */
  ALWAYSINLINE void* operator new(size_t instance_size, size_t commit_size , char* requested_addr) throw() {
    assert(commit_size > instance_size, "Committed size is too small. ");

    // discard the parameter, size, which is defined by sizeof(clas)
    return (void*)commit_at(commit_size, mtGC, requested_addr);
  }



 /**
  * new object instance #2.
  * Allocate object instances based on Alloc_type and index.
  * 
  * [X] Each object instance should be equal here.
  * 
  */
  ALWAYSINLINE void* operator new(size_t instance_size, size_t index ) throw() {
    // Calculate the queue's entire size, 4KB alignment
    // The commit_size for every queue type is fixed.
    size_t commit_size = align_up(instance_size, RDMA_ALIGNMENT_BYTES); // need to record how much memory is used
    char* requested_addr = NULL;
    char* old_val;
    char* ret;
    switch(Alloc_type)  // based on the instantiation of Template
    {
      case CPU_TO_MEM_AT_INIT_ALLOCTYPE :
        // 1) commit all the reserved space for easy debuging
        //    First time entering the zone.
        if(CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr == NULL){
          requested_addr = (char*)(SEMERU_START_ADDR + CPU_TO_MEMORY_INIT_OFFSET);
          CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_instance_size = commit_size;  // init here, compared latter.
          
          if( (char*)commit_at(CPU_TO_MEMORY_INIT_SIZE_LIMIT, mtGC, requested_addr) ==  requested_addr ){
            old_val = CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr;  // NULL
            ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr), old_val);
            assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
          }

          
          log_debug(semeru, alloc)("Initialize _alloc_ptr to 0x%lx for CPU_TO_MEM_AT_INIT_ALLOCTYPE.", 
                                                                (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr);

          break;
        }

        // 2) this type space is already committed.
        //    Just return a start address back to caller && adjust the pointer value.
        requested_addr = CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr;
        assert((size_t)(requested_addr + commit_size) < (size_t)(SEMERU_START_ADDR + CPU_TO_MEMORY_INIT_OFFSET + CPU_TO_MEMORY_INIT_SIZE_LIMIT), 
                                                        "%s, Exceed the CPU_TO_MEMORY_INIT_SIZE_LIMIT's space range. \n", __func__ );

        // Before bump the alloc pointer.
        // Assume the the instance size are all the same for all the type under CPU_TO_MEM_AT_INIT_ALLOCTYPE
        // assert( (size_t)(SEMERU_START_ADDR + CPU_TO_MEMORY_INIT_OFFSET + index * commit_size) == (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr,
        //     " Each element size of type  CPU_TO_MEM_AT_INIT_ALLOCTYPE should be equal.");

        // bump the pointer
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr), requested_addr);
        assert(ret == requested_addr, "%s, Not MT Safe. \n", __func__);

        log_debug(semeru, alloc)("Bump _alloc_ptr to 0x%lx for CPU_TO_MEM_AT_INIT_ALLOCTYPE.", 
                                                                (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_INIT_ALLOCTYPE>::_alloc_ptr);

        break;


      case CPU_TO_MEM_AT_GC_ALLOCTYPE :
        // 1) commit all the reserved space for easy debuging
        //    First time entering the zone.
        if(CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr == NULL){
          requested_addr = (char*)(SEMERU_START_ADDR + CPU_TO_MEMORY_GC_OFFSET);
          CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_instance_size = commit_size;  // init here, compared latter.

          
          if( (char*)commit_at(CPU_TO_MEMORY_GC_SIZE_LIMIT, mtGC, requested_addr) ==  requested_addr ){
            old_val = CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr;  // NULL
            ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr), old_val);
            assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
          }

          
          log_debug(semeru, alloc)("Initialize _alloc_ptr to 0x%lx for CPU_TO_MEM_AT_GC_ALLOCTYPE.", 
                                                                (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr);

          break;
        }

        // 2) this type space is already committed.
        //    Just return a start address back to caller && adjust the pointer value.
        requested_addr = CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr;
        assert((size_t)(requested_addr + commit_size) < (size_t)(SEMERU_START_ADDR + CPU_TO_MEMORY_GC_OFFSET + CPU_TO_MEMORY_GC_SIZE_LIMIT), 
                                                        "%s, Exceed the CPU_TO_MEMORY_INIT_SIZE_LIMIT's space range. \n", __func__ );

        // Before bump the alloc pointer.
        // Assume the the instance size are all the same for all the type under CPU_TO_MEM_AT_GC_ALLOCTYPE
        // assert( (size_t)(SEMERU_START_ADDR + CPU_TO_MEMORY_GC_OFFSET + index * commit_size) == (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr,
        //     " Each element size of type  CPU_TO_MEM_AT_GC_ALLOCTYPE should be equal.");

        // bump the pointer
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr), requested_addr);
        assert(ret == requested_addr, "%s, Not MT Safe. \n", __func__);

        log_debug(semeru, alloc)("Bump _alloc_ptr to 0x%lx for CPU_TO_MEM_AT_GC_ALLOCTYPE.", 
                                                                (size_t)CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_alloc_ptr);

        break;


      case MEM_TO_CPU_AT_GC_ALLOCTYPE :
        // 1) commit all the reserved space for easy debuging
        //    First time entering the zone.
        if(CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr == NULL){
          requested_addr = (char*)(SEMERU_START_ADDR + MEMORY_TO_CPU_GC_OFFSET);
          CHeapRDMAObj<E, CPU_TO_MEM_AT_GC_ALLOCTYPE>::_instance_size = commit_size;  // init here, compared latter.
          
          if( (char*)commit_at(MEMORY_TO_CPU_GC_SIZE_LIMIT, mtGC, requested_addr) ==  requested_addr ){
            old_val = CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr;  // NULL
            ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr), old_val);
            assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
          }

          
          log_debug(semeru, alloc)("Initialize _alloc_ptr to 0x%lx for MEM_TO_CPU_AT_GC_ALLOCTYPE.", 
                                                                (size_t)CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr);

          break;
        }

        // 2) this type space is already committed.
        //    Just return a start address back to caller && adjust the pointer value.
        requested_addr = CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr;
        assert((size_t)(requested_addr + commit_size) < (size_t)(SEMERU_START_ADDR + MEMORY_TO_CPU_GC_OFFSET + MEMORY_TO_CPU_GC_SIZE_LIMIT), 
                                                        "%s, Exceed the MEM_TO_CPU_AT_GC_ALLOCTYPE's space range. \n", __func__ );

        // Before bump the alloc pointer.
        // Assume the the instance size are all the same for all the type under MEM_TO_CPU_AT_GC_ALLOCTYPE
        // assert( (size_t)(SEMERU_START_ADDR + MEMORY_TO_CPU_GC_OFFSET + index * commit_size) == (size_t)CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr,
        //     " Each element size of type  MEM_TO_CPU_AT_GC_ALLOCTYPE should be equal.");

        // bump the pointer
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr), requested_addr);
        assert(ret == requested_addr, "%s, Not MT Safe. \n", __func__);

        log_debug(semeru, alloc)("Bump _alloc_ptr to 0x%lx for MEM_TO_CPU_AT_GC_ALLOCTYPE.", 
                                                                (size_t)CHeapRDMAObj<E, MEM_TO_CPU_AT_GC_ALLOCTYPE>::_alloc_ptr);

        break;



      case NON_ALLOC_TYPE :
          //debug
          tty->print("Error in %s. Can't reach here. \n",__func__);
        break;


      default:
        requested_addr = NULL;
        break;
    }



    // discard the parameter, size, which is defined by sizeof(clas)
    return (void*)requested_addr;
  }







  /**
   * new object array #1
   * 
   * Allocate space for Object Array.
   *  1) multiple queue with index information.
   *  2) Content is not a flexible array.
   *  3) Fixed start_addr.
   * 
   * Parameters
   *  size, the instance size 
   *  element_size,  the size of 
   *  index, 
   *  Alloc_type, which kind of RDMA meta data is using.
   * 
   * MT Safe :
   *  Make this allocation Multiple-Thread safe ?
   *  
   * 
   */
  ALWAYSINLINE void* operator new(size_t instance_size, size_t element_length, size_t index ) throw() {
    // Calculate the queue's entire size, 4KB alignment
    // The commit_size for every queue type is fixed.
    size_t commit_size = commit_size_for_queue(instance_size, element_length); // need to record how much memory is used
    char* requested_addr = NULL;
    char* old_val;
    char* ret;
    switch(Alloc_type)  // based on the instantiation of Template
    {
      case ALLOC_TARGET_OBJ_QUEUE :
        requested_addr = (char*)(SEMERU_START_ADDR + TARGET_OBJ_OFFSET + index * commit_size) ;

        assert(requested_addr + commit_size < (char*)(SEMERU_START_ADDR + TARGET_OBJ_OFFSET +TARGET_OBJ_SIZE_BYTE), 
                                                        "%s, Exceed the TARGET_OBJ_QUEUE's space range. \n", __func__ );
        
        // [?] How to handle the failure ?
        //     The _alloc_ptr is useless here.
        old_val = CHeapRDMAObj<E, ALLOC_TARGET_OBJ_QUEUE>::_alloc_ptr;
        ret = Atomic::cmpxchg(requested_addr + commit_size, &(CHeapRDMAObj<E, ALLOC_TARGET_OBJ_QUEUE>::_alloc_ptr), old_val);
        assert(ret == old_val, "%s, Not MT Safe. \n",__func__);
        
        break;

      case NON_ALLOC_TYPE :
          //debug
          tty->print("Error in %s. Can't reach here. \n",__func__);
        break;

      default:
        requested_addr = NULL;
        break;
    }



    // discard the parameter, size, which is defined by sizeof(clas)
    return (void*)commit_at(commit_size, mtGC, requested_addr);
  }


  // commit space on reserved space
 // static char* commit_at(size_t length, MEMFLAGS flags, char* requested_addr)

  static E*     commit_at(size_t commit_byte_size, MEMFLAGS flags, char* requested_addr);
  static size_t commit_size_for_queue(size_t instance_size, size_t elem_length); 
  // debug
  static E* test_new_operator( size_t size, size_t commit_size, char* requested_addr);
};






// https://stackoverflow.com/questions/610245/where-and-why-do-i-have-to-put-the-template-and-typename-keywords
// Each instantiation of the tmplate class has different instance of static variable
// operator new is invoked before the constructor,
// so it's useless to initiate the value here.
// We assign the values in operator new.
template <class E , CHeapAllocType Alloc_type>
char* CHeapRDMAObj<E, Alloc_type>::_alloc_ptr = NULL;


template <class E , CHeapAllocType Alloc_type>
size_t CHeapRDMAObj<E, Alloc_type>::_instance_size = 0;



/**
 * Semeru - Support RDMA data transfer for this structure.
 *  
 * 		1) This object should be allocated at fixed address.
 * 		2) Parts of its fields also allocated at fixe address, just behind the instance.
 * 
 */
template <class E, unsigned int N, CHeapAllocType Alloc_type>
class TaskQueueRDMASuper: public CHeapRDMAObj<E, Alloc_type> {
protected:
  // Internal type for indexing the queue; also used for the tag.
  typedef NOT_LP64(uint16_t) LP64_ONLY(uint32_t) idx_t;

  // The first free element after the last one pushed (mod N).
  volatile uint _bottom;  // [x] points to the first free/available slot.

  enum { MOD_N_MASK = N - 1 };

  // Tag : Used for work stealing. 
  // If other threads steal an elem by pop_global, _age.top++.
  //
  class Age {
  public:
    Age(size_t data = 0)         { _data = data; }
    Age(const Age& age)          { _data = age._data; }
    Age(idx_t top, idx_t tag)    { _fields._top = top; _fields._tag = tag; }

    Age   get()        const volatile { return _data; }
    void  set(Age age) volatile       { _data = age._data; }

    idx_t top()        const volatile { return _fields._top; }
    idx_t tag()        const volatile { return _fields._tag; }

    // Increment top; if it wraps, increment tag also.
    void increment() {
      _fields._top = increment_index(_fields._top);
      if (_fields._top == 0) ++_fields._tag;
    }

    Age cmpxchg(const Age new_age, const Age old_age) volatile;

    bool operator ==(const Age& other) const { return _data == other._data; }

  private:

    // [?] What's the purpose of _top and _tag ?
    //
    struct fields {
      idx_t _top;
      idx_t _tag;
    };

    union {
      size_t _data;
      fields _fields;
    };
  };

  volatile Age _age;  // Used for work streal. _age._top points to the first inserted item.




	// End of fields.


  // These both operate mod N.
  static uint increment_index(uint ind) {
    return (ind + 1) & MOD_N_MASK;
  }
  static uint decrement_index(uint ind) {
    return (ind - 1) & MOD_N_MASK;
  }

  // Returns a number in the range [0..N).  If the result is "N-1", it should be
  // interpreted as 0.
  //
  // [?] dirty_size means the pushed and not handled element size ??
  //
  uint dirty_size(uint bot, uint top) const {
    return (bot - top) & MOD_N_MASK;    // -1 & MOD_N_MASK  = MOD_N_MASK (N-1).
  }

  // Returns the size corresponding to the given "bot" and "top".
  //
  // [?] Same with dirty_size, except for the boundary case. 
  //
  uint size(uint bot, uint top) const {
    uint sz = dirty_size(bot, top);     // (_bottom - _top) & MOD_N_MASK
    // Has the queue "wrapped", so that bottom is less than top?  There's a
    // complicated special case here.  A pair of threads could perform pop_local
    // and pop_global operations concurrently, starting from a state in which
    // _bottom == _top+1.  The pop_local could succeed in decrementing _bottom,
    // and the pop_global in incrementing _top (in which case the pop_global
    // will be awarded the contested queue element.)  The resulting state must
    // be interpreted as an empty queue.  (We only need to worry about one such
    // event: only the queue owner performs pop_local's, and several concurrent
    // threads attempting to perform the pop_global will all perform the same
    // CAS, and only one can succeed.)  Any stealing thread that reads after
    // either the increment or decrement will see an empty queue, and will not
    // join the competitors.  The "sz == -1 || sz == N-1" state will not be
    // modified by concurrent queues, so the owner thread can reset the state to
    // _bottom == top so subsequent pushes will be performed normally.
    return (sz == N - 1) ? 0 : sz;
  }

public:
  TaskQueueRDMASuper() : _bottom(0), _age() {}

  // Return true if the TaskQueue contains/does not contain any tasks.
  bool peek()     const { return _bottom != _age.top(); }
  bool is_empty() const { return size() == 0; }

  // Return an estimate of the number of elements in the queue.
  // The "careful" version admits the possibility of pop_local/pop_global
  // races.
  uint size() const {
    return size(_bottom, _age.top());
  }

  uint dirty_size() const {
    return dirty_size(_bottom, _age.top());
  }

  void set_empty() {
    _bottom = 0;
    _age.set(0);
  }

  // Maximum number of elements allowed in the queue.  This is two less
  // than the actual queue size, for somewhat complicated reasons.
  uint max_elems() const { return N - 2; }

  // Total size of queue.
  static const uint total_size() { return N; }

  TASKQUEUE_STATS_ONLY(TaskQueueStats stats;)
};




// Semeru Copy the implementation of the Orginal GenericTaskQueue.
// But changed their allocation policy.
//
//
// GenericTaskQueue implements an ABP, Aurora-Blumofe-Plaxton, double-
// ended-queue (deque), intended for use in work stealing. Queue operations
// are non-blocking.
//
// A queue owner thread performs push() and pop_local() operations on one end
// of the queue, while other threads may steal work using the pop_global()
// method.
//
// The main difference to the original algorithm is that this
// implementation allows wrap-around at the end of its allocated
// storage, which is an array.
//
// The original paper is:
//
// Arora, N. S., Blumofe, R. D., and Plaxton, C. G.
// Thread scheduling for multiprogrammed multiprocessors.
// Theory of Computing Systems 34, 2 (2001), 115-144.
//
// The following paper provides an correctness proof and an
// implementation for weakly ordered memory models including (pseudo-)
// code containing memory barriers for a Chase-Lev deque. Chase-Lev is
// similar to ABP, with the main difference that it allows resizing of the
// underlying storage:
//
// Le, N. M., Pop, A., Cohen A., and Nardell, F. Z.
// Correct and efficient work-stealing for weak memory models
// Proceedings of the 18th ACM SIGPLAN symposium on Principles and
// practice of parallel programming (PPoPP 2013), 69-80
//

template <class E, CHeapAllocType Alloc_type, unsigned int N = TASKQUEUE_SIZE>
class GenericTaskQueueRDMA: public TaskQueueRDMASuper<E, N, Alloc_type> {


protected:
  typedef typename TaskQueueRDMASuper<E, N, Alloc_type>::Age Age;       // [?] What's the Age used for ? Work steal related.
  typedef typename TaskQueueRDMASuper<E, N, Alloc_type>::idx_t idx_t;   // [?] purpose ?

  using TaskQueueRDMASuper<E, N, Alloc_type>::_bottom;            // points to the first available slot.
  using TaskQueueRDMASuper<E, N, Alloc_type>::_age;               // _age._top, points to the first inserted item. Usd by working steal.
  using TaskQueueRDMASuper<E, N, Alloc_type>::increment_index;
  using TaskQueueRDMASuper<E, N, Alloc_type>::decrement_index;
  using TaskQueueRDMASuper<E, N, Alloc_type>::dirty_size;         // [?] Definition of dirty ??

public:
  using TaskQueueRDMASuper<E, N, Alloc_type>::max_elems;      // N-2, 2 slots are reserved for "complicated" reasons.
  using TaskQueueRDMASuper<E, N, Alloc_type>::size;           // _bottom - _top

#if  TASKQUEUE_STATS
  using TaskQueueRDMASuper<E, N, Alloc_type>::stats;
#endif

private:
  // Slow paths for push, pop_local.  (pop_global has no fast path.)
  bool push_slow(E t, uint dirty_n_elems);
  bool pop_local_slow(uint localBot, Age oldAge);

public:
  typedef E element_type;

  // Initializes the queue to empty.
  GenericTaskQueueRDMA();

  void initialize();

  // Push the task "t" on the queue.  Returns "false" iff the queue is full.
  inline bool push(E t);

  // Attempts to claim a task from the "local" end of the queue (the most
  // recently pushed) as long as the number of entries exceeds the threshold.
  // If successful, returns true and sets t to the task; otherwise, returns false
  // (the queue is empty or the number of elements below the threshold).
  inline bool pop_local(volatile E& t, uint threshold = 0);

  // Like pop_local(), but uses the "global" end of the queue (the least
  // recently pushed).
  bool pop_global(volatile E& t);

  // Delete any resource associated with the queue.
  ~GenericTaskQueueRDMA();

  // Apply fn to each element in the task queue.  The queue must not
  // be modified while iterating.
  template<typename Fn> void iterate(Fn fn);

  // Promote to public
  volatile E* _elems;       // [x] The real content, class pointer buffer, of the GenericTaskQueueRDMA. 
  
private:
  DEFINE_PAD_MINUS_SIZE(0, DEFAULT_CACHE_LINE_SIZE, 0);
  // Element array.
  //volatile E* _elems;       // [x] The real content, buffer, of the GenericTaskQueue. 

  DEFINE_PAD_MINUS_SIZE(1, DEFAULT_CACHE_LINE_SIZE, sizeof(E*));
  // Queue owner local variables. Not to be accessed by other threads.

  static const uint InvalidQueueId = uint(-1);
  uint _last_stolen_queue_id; // The id of the queue we last stole from

  int _seed; // Current random seed used for selecting a random queue during stealing.

  DEFINE_PAD_MINUS_SIZE(2, DEFAULT_CACHE_LINE_SIZE, sizeof(uint) + sizeof(int));
public:
  int next_random_queue_id();

  void set_last_stolen_queue_id(uint id)     { _last_stolen_queue_id = id; }
  uint last_stolen_queue_id() const          { return _last_stolen_queue_id; }
  bool is_last_stolen_queue_id_valid() const { return _last_stolen_queue_id != InvalidQueueId; }
  void invalidate_last_stolen_queue_id()     { _last_stolen_queue_id = InvalidQueueId; }
};

// The constructor of GenericTaskQueueRDMA
template<class E, CHeapAllocType Alloc_type, unsigned int N>
GenericTaskQueueRDMA<E, Alloc_type, N>::GenericTaskQueueRDMA() : 
_last_stolen_queue_id(InvalidQueueId), 
_seed(17 /* random number */) {
  assert(sizeof(Age) == sizeof(size_t), "Depends on this.");
}








/**
 *  Target Object Queue, same as OverflowTaskQueue. But it's allocated at specific address.
 *  This strcuture is transfered by RDMA.
 *  
 *  [x] The last field of current class is _overflow_stack, not a flexible array.
 * 
 */
template<class E, CHeapAllocType Alloc_type, unsigned int N = TASKQUEUE_SIZE>
class OverflowTargetObjQueue: public GenericTaskQueueRDMA<E, Alloc_type, N>
{
public:
  typedef Stack<E, mtGC>               overflow_t;     // Newly added overflow stack. no size limitation ??
  typedef GenericTaskQueueRDMA<E, Alloc_type, N> taskqueue_t;    // Content is E* _elem, length is N, Mem_type is F.

  // The start address for current OverflowTargetObjQueue instance, is this.
  // GenericTaskQueue->(E*)_elems is the Content for every single GenericTaskQueue<E, F, N> taskqueue_t.
  size_t  _region_index; // Corresponding the HeapRegion->index.

  TASKQUEUE_STATS_ONLY(using taskqueue_t::stats;)

  OverflowTargetObjQueue(); // Constructor.

  // newly defined initialize() function for space allocation.
  // Commit space for the GenericTaskQueueRDMA->(Class E*)_elem, just behind current class instance.
  void initialize(size_t q_index);  

  // Push task t onto the queue or onto the overflow stack.  Return true.
  inline bool push(E t);
  // Try to push task t onto the queue only. Returns true if successful, false otherwise.
  inline bool try_push_to_taskqueue(E t);

  // Attempt to pop from the overflow stack; return true if anything was popped.
  inline bool pop_overflow(E& t);

  inline overflow_t* overflow_stack() { return &_overflow_stack; }

  inline bool taskqueue_empty() const { return taskqueue_t::is_empty(); }
  inline bool overflow_empty()  const { return _overflow_stack.is_empty(); }
  inline bool is_empty()        const {
    return taskqueue_empty() && overflow_empty();
  }

private:
  overflow_t _overflow_stack;     // The Stack<E,F>
};





/**
 * Semeru 
 *  
 *  CPU Server  - Producer 
 *     CPU server builds the TargetObjQueue from 3 roots. And send the TargetQueue to Memory sever at the end of each CPU server GC.
 *     First, from thread stack variables. This is done during CPU server GC.
 *     Second, Cross-Region references recoreded by the Post Write Barrier ?
 *     Third, the SATB buffer queue, recoreded by the Pre Write Barrier.
 *  
 *  Memory Server - Consumer 
 *     Receive the TargetObjQueue and use them as the scavenge roots.
 * 
 */
 typedef OverflowTargetObjQueue<StarTask, ALLOC_TARGET_OBJ_QUEUE>        TargetObjQueue;     // Override the typedef of OopTaskQueue
 //typedef GenericTaskQueueSet<TargetObjQueue, mtGC>     TargetObjQueueSet;  // Assign to a global ?





/**
 * Limit the instance size within 4KB 
 * 
 * size_t, unsigned long, 8 bytes.
 * 
 * 1) CPU server send data to rewrite the value of _num_regions and _regions_cset[].
 * 		This instance has to be allocated in a fixed memory range, reserved by both CPU server and memory server.
 * 2) Use _num_regions as flag of if new data are written here by CPU server, producer.
 * 		Also use _num_regions to index the content of _region_cset[], decreased by Memory server, consumer.
 * 3) This function is not MT safe. We ausme that only one thread can invoke this function.
 * 
 */
class received_memory_server_cset : public CHeapRDMAObj<received_memory_server_cset>{

private :
	// First field, identify if CPU server pushed new Regions here.
	size_t 	_num_regions;

	// [?] a flexible array, points the memory just behind this instance.
	// The size of current instance should be limited within 4K, 
	// The array size should be limited by MEM_SERVER_CSET_BUFFER_SIZE.
	size_t	_region_cset[];			



public :
	
	received_memory_server_cset();
	
	size_t*	num_received_regions()	{	return &_num_regions;	}


	// Allocate the _region_cset just behind this instance ?
	// void initialize(){
	// 	_num_regions = 0;
	// }



	void reset()	{	_num_regions = 0;	}

	//
	// This function isn't MT safe.
	// 
	int	pop()	{
		if(_num_regions >= 1)
			return _region_cset[--_num_regions];	
		else
			return -1; 
	}

};


/**
 * Memory server need to know the current state of CPU srever to make its own decesion.
 * For example, if the CPU server is in STW GC now, memory server switch to Remark and Compact cm_scanned Regions.
 * Or memory server keeps concurrently tracing the freshly evicted Regions.
 *  
 * 	These flags are only writable by CPU server.
 *  Memory server only read the value of them.
 *  These varialbes are valotile, every time Memory server needs to read the value from memory.
 * 
 */
class flags_of_cpu_server_state : public CHeapRDMAObj<flags_of_cpu_server_state>{
	private :
		volatile bool _is_cpu_server_in_stw;

	public :
		flags_of_cpu_server_state();

		inline bool is_cpu_server_in_stw()	{	return _is_cpu_server_in_stw;	}

};







/**
 * Semeru Memory Server
 *  
 *  This class is used to pad gap between two RDMA structure.
 * 
 */
class rdma_padding : public CHeapRDMAObj<rdma_padding, NON_ALLOC_TYPE>{
public:
  size_t num_of_item;
  char* content[];
};




#endif // SHARE_GC_SHARED_RDMA_STRUCTURE