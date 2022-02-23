#ifndef _LINUX_PAGE_REF_H
#define _LINUX_PAGE_REF_H

#include <linux/atomic.h>
#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/tracepoint-defs.h>

extern struct tracepoint __tracepoint_page_ref_set;
extern struct tracepoint __tracepoint_page_ref_mod;
extern struct tracepoint __tracepoint_page_ref_mod_and_test;
extern struct tracepoint __tracepoint_page_ref_mod_and_return;
extern struct tracepoint __tracepoint_page_ref_mod_unless;
extern struct tracepoint __tracepoint_page_ref_freeze;
extern struct tracepoint __tracepoint_page_ref_unfreeze;

#ifdef CONFIG_DEBUG_PAGE_REF

/*
 * Ideally we would want to use the trace_<tracepoint>_enabled() helper
 * functions. But due to include header file issues, that is not
 * feasible. Instead we have to open code the static key functions.
 *
 * See trace_##name##_enabled(void) in include/linux/tracepoint.h
 */
#define page_ref_tracepoint_active(t) static_key_false(&(t).key)

extern void __page_ref_set(struct page *page, int v);
extern void __page_ref_mod(struct page *page, int v);
extern void __page_ref_mod_and_test(struct page *page, int v, int ret);
extern void __page_ref_mod_and_return(struct page *page, int v, int ret);
extern void __page_ref_mod_unless(struct page *page, int v, int u);
extern void __page_ref_freeze(struct page *page, int v, int ret);
extern void __page_ref_unfreeze(struct page *page, int v);

#else

#define page_ref_tracepoint_active(t) false

static inline void __page_ref_set(struct page *page, int v)
{
}
static inline void __page_ref_mod(struct page *page, int v)
{
}
static inline void __page_ref_mod_and_test(struct page *page, int v, int ret)
{
}
static inline void __page_ref_mod_and_return(struct page *page, int v, int ret)
{
}
static inline void __page_ref_mod_unless(struct page *page, int v, int u)
{
}
static inline void __page_ref_freeze(struct page *page, int v, int ret)
{
}
static inline void __page_ref_unfreeze(struct page *page, int v)
{
}

#endif

static inline int page_ref_count(struct page *page)
{
	return atomic_read(&page->_refcount);
}

static inline int page_count(struct page *page)
{
	return atomic_read(&compound_head(page)->_refcount);  // return the page->_refcount directly ?
}

static inline void set_page_count(struct page *page, int v)
{
	atomic_set(&page->_refcount, v);
	if (page_ref_tracepoint_active(__tracepoint_page_ref_set))
		__page_ref_set(page, v);
}

/*
 * Setup the page count before being freed into the page allocator for
 * the first time (boot or memory hotplug)
 * 
 * [?] Why set it to 1 ? it's initial value should be -1 ?
 * 
 */
static inline void init_page_count(struct page *page)
{
	set_page_count(page, 1);
}

static inline void page_ref_add(struct page *page, int nr)
{
	atomic_add(nr, &page->_refcount);
	if (page_ref_tracepoint_active(__tracepoint_page_ref_mod))
		__page_ref_mod(page, nr);
}

static inline void page_ref_sub(struct page *page, int nr)
{
	atomic_sub(nr, &page->_refcount);
	if (page_ref_tracepoint_active(__tracepoint_page_ref_mod))
		__page_ref_mod(page, -nr);
}

static inline void page_ref_inc(struct page *page)
{
	atomic_inc(&page->_refcount);
	if (page_ref_tracepoint_active(__tracepoint_page_ref_mod))
		__page_ref_mod(page, 1);
}

static inline void page_ref_dec(struct page *page)
{
	atomic_dec(&page->_refcount);
	if (page_ref_tracepoint_active(__tracepoint_page_ref_mod))
		__page_ref_mod(page, -1);
}

static inline int page_ref_sub_and_test(struct page *page, int nr)
{
	int ret = atomic_sub_and_test(nr, &page->_refcount);

	if (page_ref_tracepoint_active(__tracepoint_page_ref_mod_and_test))
		__page_ref_mod_and_test(page, -nr, ret);
	return ret;
}

static inline int page_ref_inc_return(struct page *page)
{
	int ret = atomic_inc_return(&page->_refcount);

	if (page_ref_tracepoint_active(__tracepoint_page_ref_mod_and_return))
		__page_ref_mod_and_return(page, 1, ret);
	return ret;
}

/**
 * decrease the page->_refcount and check if its value is 0.
 * 
 * [?] will this really change the value of page->_refcount ?
 * or just compare the page->_refcount -1  and 0 ? 
 * 	it decreased the page->_refcount.
 *  if the value is changed to 0.
 *  Then inovoke free_hot_cold_pages_list() to free them directly
 * 
 * [x] What's connection between page->_refcount and page->_mapcount ?
 * 	page->_mapcount: User Space mapping. the number of mapping from user space process/pte
 * 	page->_refcount: include page->_mapcount. also includes kernel structure, e.g. DMA, and any other structure claim the page by get_page().
 * 
 * 
 */
static inline int page_ref_dec_and_test(struct page *page)
{
	int ret = atomic_dec_and_test(&page->_refcount);  // decrease the _refcount

	if (page_ref_tracepoint_active(__tracepoint_page_ref_mod_and_test))
		__page_ref_mod_and_test(page, -1, ret);
	return ret;
}

static inline int page_ref_dec_return(struct page *page)
{
	int ret = atomic_dec_return(&page->_refcount);

	if (page_ref_tracepoint_active(__tracepoint_page_ref_mod_and_return))
		__page_ref_mod_and_return(page, -1, ret);
	return ret;
}

static inline int page_ref_add_unless(struct page *page, int nr, int u)
{
	int ret = atomic_add_unless(&page->_refcount, nr, u);

	if (page_ref_tracepoint_active(__tracepoint_page_ref_mod_unless))
		__page_ref_mod_unless(page, nr, ret);
	return ret;
}

static inline int page_ref_freeze(struct page *page, int count)
{
	int ret = likely(atomic_cmpxchg(&page->_refcount, count, 0) == count);

	if (page_ref_tracepoint_active(__tracepoint_page_ref_freeze))
		__page_ref_freeze(page, count, ret);
	return ret;
}

static inline void page_ref_unfreeze(struct page *page, int count)
{
	VM_BUG_ON_PAGE(page_count(page) != 0, page);
	VM_BUG_ON(count == 0);

	atomic_set(&page->_refcount, count);
	if (page_ref_tracepoint_active(__tracepoint_page_ref_unfreeze))
		__page_ref_unfreeze(page, count);
}


//
// Semeru 

/**
 * @brief 
 * 
 * @param page 
 * @param count : the expected old value? 
 * @return 1 : can be freed, repalce the page->_refcount to 0 from 2.
 * 0 : cannot be freed.
 */
static inline int semeru_page_ref_freeze(struct page *page, int count)
{
	int ret = likely(atomic_cmpxchg(&page->_refcount, count, 0) == count);

	//debug
	if(!ret){
		pr_err("%s:%d, page->_refcount is %d, cannot be freed. \n", __func__, __LINE__, page->_refcount.counter);
	}


	if (page_ref_tracepoint_active(__tracepoint_page_ref_freeze))
		__page_ref_freeze(page, count, ret);
	return ret;
}


#endif
