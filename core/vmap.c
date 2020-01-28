#include <lib/inthash.h>
#include <memory.h>
#include <object.h>
#include <processor.h>
#include <slab.h>

struct vm_context kernel_ctx = {};

struct slabcache sc_vmctx, sc_vmap;

static void _vmctx_ctor(void *_p, void *obj)
{
	(void)_p;
	struct vm_context *v = obj;
	arch_mm_context_init(v);
}

static void _vmctx_dtor(void *_p, void *obj)
{
	(void)_p;
	(void)obj;
}

__initializer static void _init_vmctx(void)
{
	slabcache_init(&sc_vmap, sizeof(struct vmap), NULL, NULL, NULL);
	slabcache_init(&sc_vmctx, sizeof(struct vm_context), _vmctx_ctor, _vmctx_dtor, NULL);
}

struct vm_context *vm_context_create(void)
{
	return slabcache_alloc(&sc_vmctx);
}

void vm_context_destroy(struct vm_context *v)
{
	/* TODO (major): unmap things? */
	slabcache_free(v);
}

static int vmap_compar_key(struct vmap *v, size_t slot)
{
	if(v->slot > slot)
		return 1;
	if(v->slot < slot)
		return -1;
	return 0;
}

static int vmap_compar(struct vmap *a, struct vmap *b)
{
	return vmap_compar_key(a, b->slot);
}

void vm_vmap_init(struct vmap *vmap, struct object *obj, size_t vslot, uint32_t flags)
{
	vmap->slot = vslot;
	krc_get(&obj->refs);
	vmap->obj = obj;
	vmap->flags = flags;
	vmap->status = 0;
}

void vm_context_map(struct vm_context *v, struct vmap *m)
{
	if(!rb_insert(&v->root, m, struct vmap, node, vmap_compar)) {
		panic("Map already exists");
	}
}

void kso_view_write(struct object *obj, size_t slot, struct viewentry *v)
{
	obj_write_data(obj, __VE_OFFSET + slot * sizeof(struct viewentry), sizeof(struct viewentry), v);
}

struct viewentry kso_view_lookup(struct vm_context *ctx, size_t slot)
{
	struct viewentry v;
	obj_read_data(kso_get_obj(ctx->view, view),
	  __VE_OFFSET + slot * sizeof(struct viewentry),
	  sizeof(struct viewentry),
	  &v);
	return v;
}

#include <twz/_thrd.h>
static bool lookup_by_slot(size_t slot, objid_t *id, uint64_t *flags)
{
	switch(slot) {
		struct viewentry ve;
		// case 0x10000:
		//	*id = kso_get_obj(current_thread->throbj, thr)->id;
		//	if(flags) *flags = VE_READ | VE_WRITE;
		// break;
		default:
			obj_read_data(kso_get_obj(current_thread->throbj, thr),
			  slot * sizeof(struct viewentry) + sizeof(struct twzthread_repr),
			  sizeof(struct viewentry),
			  &ve);
			if(ve.res0 == 0 && ve.res1 == 0 && ve.flags & VE_VALID) {
				//			printk("Slot %lx is fixed-point " IDFMT " %x\n", slot, IDPR(ve.id),
				// ve.flags);
				*id = ve.id;
				if(flags)
					*flags = ve.flags;
				return true;
			}
			ve = kso_view_lookup(current_thread->ctx, slot);
			//		printk("Slot %lx contains " IDFMT " %x\n", slot, IDPR(ve.id), ve.flags);
			if(ve.res0 != 0 || ve.res1 != 0 || !(ve.flags & VE_VALID)) {
				return false;
			}
			*id = ve.id;
			if(flags)
				*flags = ve.flags;
	}
	return true;
}

bool vm_vaddr_lookup(void *addr, objid_t *id, uint64_t *off)
{
	size_t slot = (uintptr_t)addr / mm_page_size(MAX_PGLEVEL);
	uint64_t o = (uintptr_t)addr % mm_page_size(MAX_PGLEVEL);

	if(off)
		*off = o;
	return lookup_by_slot(slot, id, NULL);
}

static bool _vm_view_invl(struct object *obj, struct kso_invl_args *invl)
{
	spinlock_acquire_save(&current_thread->ctx->lock);
	for(size_t slot = invl->offset / mm_page_size(MAX_PGLEVEL);
	    slot <= (invl->offset + invl->length) / mm_page_size(MAX_PGLEVEL);
	    slot++) {
		struct rbnode *node =
		  rb_search(&current_thread->ctx->root, slot, struct vmap, node, vmap_compar_key);
		/* TODO (major): unmap all ctxs that use this view */
		if(node) {
			struct vmap *map = rb_entry(node, struct vmap, node);
			arch_vm_unmap_object(current_thread->ctx, map, obj);
			rb_delete(node, &current_thread->ctx->root);
		}
	}
	spinlock_release_restore(&current_thread->ctx->lock);
	return true;
}

bool vm_setview(struct thread *t, struct object *viewobj)
{
	obj_kso_init(viewobj, KSO_VIEW); // TODO
	// struct object *old = (t->ctx && t->ctx->view) ? kso_get_obj(t->ctx->view, view) : NULL;
	// struct vm_context *oldctx = t->ctx;
	t->ctx = vm_context_create();
	krc_get(&viewobj->refs);
	t->ctx->view = &viewobj->view;

	/* TODO: unmap things (or create a new context), destroy old, etc */
	/* TODO: check object type */
	return true;
}

static struct kso_calls _kso_view = {
	.ctor = NULL,
	.dtor = NULL,
	.attach = NULL,
	.detach = NULL,
	.invl = _vm_view_invl,
};

__initializer static void _init_kso_view(void)
{
	kso_register(KSO_VIEW, &_kso_view);
}

static inline void popul_info(struct fault_object_info *info,
  int flags,
  uintptr_t ip,
  uintptr_t addr,
  objid_t objid)
{
	memset(info, 0, sizeof(*info));
	if(!(flags & FAULT_ERROR_PERM)) {
		info->flags |= FAULT_OBJECT_NOMAP;
	}
	if(flags & FAULT_WRITE) {
		info->flags |= FAULT_OBJECT_WRITE;
	} else {
		info->flags |= FAULT_OBJECT_READ;
	}
	if(flags & FAULT_EXEC) {
		info->flags |= FAULT_OBJECT_EXEC;
	}
	info->ip = ip;
	info->addr = addr;
	info->objid = objid;
}

void vm_context_fault(uintptr_t ip, uintptr_t addr, int flags)
{
	// printk("Page Fault from %lx: %lx %x\n", ip, addr, flags);

	if(flags & FAULT_ERROR_PERM) {
		struct fault_object_info info;
		popul_info(&info, flags, ip, addr, 0);
		thread_raise_fault(current_thread, FAULT_OBJECT, &info, sizeof(info));
		return;
	}
	size_t slot = addr / mm_page_size(MAX_PGLEVEL);
	struct vmap *map = NULL;
	spinlock_acquire_save(&current_thread->ctx->lock);
	struct rbnode *node =
	  rb_search(&current_thread->ctx->root, slot, struct vmap, node, vmap_compar_key);
	if(node) {
		map = rb_entry(node, struct vmap, node);
	}
	if(!map) {
		objid_t id;
		uint64_t fl;
		if(!lookup_by_slot(slot, &id, &fl)) {
			struct fault_object_info info;
			popul_info(&info, flags, ip, addr, 0);
			thread_raise_fault(current_thread, FAULT_OBJECT, &info, sizeof(info));
			spinlock_release_restore(&current_thread->ctx->lock);
			return;
		}
		map = slabcache_alloc(&sc_vmap);
		struct object *obj = obj_lookup(id);
		if(!obj) {
			struct fault_object_info info;
			popul_info(&info, flags, ip, addr, id);
			info.flags |= FAULT_OBJECT_EXIST;
			thread_raise_fault(current_thread, FAULT_OBJECT, &info, sizeof(info));
			spinlock_release_restore(&current_thread->ctx->lock);
			return;
		}
		vm_vmap_init(map, obj, slot, fl & (VE_READ | VE_WRITE | VE_EXEC));
		obj_put(obj);
		vm_context_map(current_thread->ctx, map);
	}
	spinlock_release_restore(&current_thread->ctx->lock);
	if(map->obj->slot == NULL) {
		obj_alloc_slot(map->obj);
	}
	arch_vm_map_object(current_thread->ctx, map, map->obj);
}
