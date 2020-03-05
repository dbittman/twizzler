#include <arena.h>
#include <memory.h>
#include <page.h>
#include <slab.h>

struct page_stack {
	struct spinlock lock, lock2;
	struct page *top;
	struct page *top_z;
	_Atomic size_t avail, nzero;
	_Atomic bool adding;
};

struct page_stack _stacks[MAX_PGLEVEL + 1];

size_t mm_page_count = 0;
size_t mm_page_alloc_count = 0;
_Atomic size_t mm_page_alloced = 0;
size_t mm_page_bootstrap_count = 0;

static struct arena page_arena;

static struct page *__do_page_alloc(struct page_stack *stack, bool zero)
{
	struct page **top = zero ? &stack->top_z : &stack->top;
	struct page *ret = *top;
	if(!ret)
		return NULL;
	*top = (*top)->next;
	ret->next = NULL;

	stack->avail--;
	if(zero)
		stack->nzero--;
	return ret;
}

static void __do_page_dealloc(struct page_stack *stack, struct page *page)
{
	struct page **top = (page->flags & PAGE_ZERO) ? &stack->top_z : &stack->top;
	page->next = *top;
	*top = page;
	stack->avail++;
	if(page->flags & PAGE_ZERO)
		stack->nzero++;
}

void page_print_stats(void)
{
	printk("page bootstrap count: %ld (%ld KB; %ld MB)\n",
	  mm_page_bootstrap_count,
	  (mm_page_bootstrap_count * mm_page_size(0)) / 1024,
	  (mm_page_bootstrap_count * mm_page_size(0)) / (1024 * 1024));
	printk("page alloced: %ld (%ld KB; %ld MB)\n",
	  mm_page_alloced,
	  (mm_page_alloced * mm_page_size(0)) / 1024,
	  (mm_page_alloced * mm_page_size(0)) / (1024 * 1024));

	printk("page count: %ld (%ld KB; %ld MB)\n",
	  mm_page_count,
	  (mm_page_count * mm_page_size(0)) / 1024,
	  (mm_page_count * mm_page_size(0)) / (1024 * 1024));

	for(int i = 0; i <= MAX_PGLEVEL; i++) {
		printk(
		  "page stack %d (%ld KB): %ld available\n", i, mm_page_size(i) / 1024, _stacks[i].avail);
	}
}

void page_init_bootstrap(void)
{
	/* bootstrap page allocator */
	struct page *pages = mm_virtual_early_alloc();
	size_t nrpages = mm_page_size(0) / sizeof(struct page);
	static bool did_init = false;
	if(!did_init) {
		for(int i = 0; i < MAX_PGLEVEL + 1; i++) {
			_stacks[i].lock = SPINLOCK_INIT;
			_stacks[i].lock2 = SPINLOCK_INIT;
			_stacks[i].top = NULL;
			_stacks[i].avail = 0;
			_stacks[i].adding = false;
		}
		arena_create(&page_arena);
		did_init = true;
	}

	for(size_t i = 0; i < nrpages; i++, pages++) {
		pages->addr = mm_physical_early_alloc();
		pages->level = 0;
		pages->flags = PAGE_CACHE_WB;
		__do_page_dealloc(&_stacks[0], pages);
		// pages->next = _stacks[0].top;
		//_stacks[0].top = pages;
		//_stacks[0].avail++;
		mm_page_bootstrap_count++;
	}
}

void page_init(struct memregion *region)
{
	uintptr_t addr = region->start;
	if(addr == 0)
		addr += mm_page_size(0);
	printk("init region %lx -> %lx (%ld KB; %ld MB)\n",
	  region->start,
	  region->start + region->length,
	  region->length / 1024,
	  region->length / (1024 * 1024));
	size_t nrpages = mm_page_size(0) / sizeof(struct page);
	size_t i = 0;
	struct page *pages = mm_memory_alloc(mm_page_size(0), PM_TYPE_DRAM, true);
	while(addr < region->length + region->start) {
		int level = MAX_PGLEVEL;
		for(; level > 0; level--) {
			if(align_up(addr, mm_page_size(level)) == addr
			   && addr + mm_page_size(level) <= region->start + region->length) {
				break;
			}
		}

		// printk("addr: %lx -> %lx ; %d\n", addr, addr + mm_page_size(level), level);
		struct page *page = &pages[i];

		page->level = level;
		page->addr = addr;
		page->flags = PAGE_CACHE_WB;
		page->parent = NULL;
		__do_page_dealloc(&_stacks[level], page);
		// page->next = _stacks[level].top;
		//_stacks[level].top = page;
		//_stacks[level].avail++;
		mm_page_count += mm_page_size(level) / mm_page_size(0);

		if(++i >= nrpages) {
			pages = mm_memory_alloc(mm_page_size(0), PM_TYPE_DRAM, true);
			i = 0;
			mm_page_alloc_count += mm_page_size(0);
		}

		addr += mm_page_size(level);
	}
}

#include <tmpmap.h>
static void page_zero(struct page *p)
{
	void *addr = tmpmap_map_page(p);
	memset(addr, 0, mm_page_size(p->level));
	tmpmap_unmap_page(addr);
	p->flags |= PAGE_ZERO;
}

void page_dealloc(struct page *p, int flags)
{
	if((flags & PAGE_ZERO) && !(p->flags & PAGE_ZERO)) {
		page_zero(p);
	}
	p->flags &= ~PAGE_ALLOCED;
	struct page_stack *stack = &_stacks[p->level];
	spinlock_acquire_save(&stack->lock);
	__do_page_dealloc(stack, p);
	spinlock_release_restore(&stack->lock);
}

#define __PAGE_NONZERO 0x4000

struct page *page_alloc(int type, int flags, int level)
{
	if(!mm_ready)
		level = 0;
	struct page_stack *stack = &_stacks[level];
	spinlock_acquire_save(&stack->lock);
	bool zero = (flags & PAGE_ZERO);
	struct page *p = __do_page_alloc(stack, zero);
	if(!p) {
		if(mm_ready) {
			if(flags & __PAGE_NONZERO) {
				if(stack->adding) {
					spinlock_release_restore(&stack->lock);
					return NULL;
				}
				goto add;
			} else {
				if(flags & PAGE_ZERO) {
					//	printk("MANUALLY ZEROING\n");
				}
			}
			p = __do_page_alloc(stack, !zero);
			if(!p) {
				panic("out of pages; level=%d", level);
			}
			if(zero) {
				spinlock_release_restore(&stack->lock);
				page_zero(p);
				spinlock_acquire_save(&stack->lock);
			}
		} else {
			page_init_bootstrap();
			spinlock_release_restore(&stack->lock);
			return page_alloc(type, flags, level);
		}
	}

	if(stack->avail < 128 && level < MAX_PGLEVEL && mm_ready && !stack->adding
	   && !(flags & PAGE_CRITICAL)) {
	add:
		stack->adding = true;
		spinlock_release_restore(&stack->lock);
		struct page *lp = page_alloc(type, 0, level + 1);
		//	printk("splitting page %lx (level %d)\n", lp->addr, level + 1);
		for(size_t i = 0; i < mm_page_size(level + 1) / mm_page_size(level); i++) {
			struct page *np = arena_allocate(&page_arena, sizeof(struct page));
			//*np = *lp;
			np->type = lp->type;
			np->flags = lp->flags & ~PAGE_ALLOCED;
			np->lock = SPINLOCK_INIT;
			np->root = RBINIT;
			np->addr = i * mm_page_size(level) + lp->addr;
			// np->addr += i * mm_page_size(level);
			np->parent = lp;
			np->next = NULL;
			np->level = level;
			// printk("  %p -> %lx (%d)\n", np, np->addr, np->level);
			spinlock_acquire_save(&stack->lock);
			__do_page_dealloc(stack, np);
			spinlock_release_restore(&stack->lock);
		}
		stack->adding = false;
		return page_alloc(type, flags, level);
	} else {
		spinlock_release_restore(&stack->lock);
	}

	// printk(":: ALL %lx\n", p->addr);
	assert(!(p->flags & PAGE_ALLOCED));
	p->flags &= ~PAGE_ZERO; // TODO: track this using VM system
	p->flags |= PAGE_ALLOCED;
	p->cowcount = 0;
	mm_page_alloced++;
	return p;
}

struct page *page_alloc_nophys(void)
{
	struct page *page = arena_allocate(&page_arena, sizeof(struct page));
	return page;
}

#include <processor.h>
static void __page_idle_zero(int level)
{
	struct page_stack *stack = &_stacks[level];
	while(((stack->nzero < stack->avail && stack->nzero < 1024) || stack->avail < 1024)
	      && !stack->adding) {
#if 0
		printk("ACTIVATE: idle zero: %d: %ld %ld %d\n",
		  level,
		  stack->avail,
		  stack->nzero,
		  processor_has_threads(current_processor));
#endif
		struct page *p = page_alloc(PAGE_TYPE_VOLATILE, __PAGE_NONZERO, level);
		if(p) {
			// page_zero(p);
			page_dealloc(p, PAGE_ZERO);
		}
		spinlock_acquire_save(&current_processor->sched_lock);
		bool br = processor_has_threads(current_processor);
		spinlock_release_restore(&current_processor->sched_lock);
		if(br)
			break;
		for(volatile int i = 0; i < 100; i++)
			arch_processor_relax();

		spinlock_acquire_save(&current_processor->sched_lock);
		br = processor_has_threads(current_processor);
		spinlock_release_restore(&current_processor->sched_lock);
		if(br)
			break;
	}
}

void page_idle_zero(void)
{
	static _Atomic int trying = 0;
	if(atomic_fetch_or(&trying, 1))
		return;
	// if(!processor_has_threads(current_processor)) {
	__page_idle_zero(0);
	//}
	// if(!processor_has_threads(current_processor)) {
	//	__page_idle_zero(1);
	//}
	trying = 0;
}
