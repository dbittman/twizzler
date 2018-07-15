#include <bstream.h>
#include <notify.h>
#include <twzname.h>

#define BSTREAM_METAHEADER (struct metaheader){ .id = BSTREAM_HEADER_ID, .len = sizeof(struct bstream_header) }

void bstream_notify_prepare(struct object *obj)
{
	struct bstream_header *bh = twz_object_findmeta(obj, BSTREAM_HEADER_ID);
	mutex_acquire(&bh->readlock);
	if(bh->head != bh->tail) {
		notify_wake_all(obj, INT_MAX, NTYPE_READ);
	}
	mutex_release(&bh->readlock);

	mutex_acquire(&bh->writelock);
	if(bh->tail != bh->head - (1ul << bh->nbits)) {
		notify_wake_all(obj, INT_MAX, NTYPE_WRITE);
	}
	mutex_release(&bh->writelock);
}

int bstream_getb(struct object *obj, unsigned fl)
{
	struct bstream_header *bh = twz_object_findmeta(obj, BSTREAM_HEADER_ID);
try_again:
	mutex_acquire(&bh->readlock);

	if(bh->head == bh->tail) {
		bh->rwait = bh->head;
		mutex_release(&bh->readlock);
		if(fl & TWZIO_NONBLOCK) return -TE_NREADY;
		notify_wait(obj, bh->rwid, bh->head);
		goto try_again;
	}

	unsigned char *buffer = obj->base;
	int r = buffer[bh->tail & ((1ul << bh->nbits)-1)];
	bh->tail++;

	notify_wake(obj, bh->wwid, INT_MAX);
	notify_wake_all(obj, INT_MAX, NTYPE_WRITE);
	mutex_release(&bh->readlock);
	return r;
}

#include <debug.h>
int bstream_putb(struct object *obj, unsigned char c, unsigned fl)
{
	struct bstream_header *bh = twz_object_findmeta(obj, BSTREAM_HEADER_ID);
try_again:
	mutex_acquire(&bh->writelock);
	
	if(bh->tail == bh->head - (1ul << bh->nbits)) {
		bh->rwait = bh->tail;
		mutex_release(&bh->writelock);
		if(fl & TWZIO_NONBLOCK) return -TE_NREADY;
		notify_wait(obj, bh->wwid, bh->tail);
		goto try_again;
	}

	char *buffer = obj->base;
	buffer[bh->head & ((1ul << bh->nbits)-1)] = c;
	bh->head++;
	notify_wake(obj, bh->rwid, INT_MAX);
	notify_wake_all(obj, INT_MAX, NTYPE_READ);
	mutex_release(&bh->writelock);
	return 0;
}

ssize_t bstream_read(struct object *obj, unsigned char *buf,
		size_t len, unsigned fl __unused)
{
	for(size_t i=0;i<len;i++) {
		if((buf[i] = bstream_getb(obj, 0)) == '\n')
			return i+1;
	}
	return len;
}

ssize_t bstream_write(struct object *obj, const unsigned char *buf,
		size_t len, unsigned fl __unused)
{
	for(size_t i=0;i<len;i++) bstream_putb(obj, buf[i], 0);
	return len;
}

#include <debug.h>
int bstream_init(struct object *obj, int nbits)
{
	struct bstream_header *bh = twz_object_addmeta(obj, BSTREAM_METAHEADER);
	debug_printf("DO WRITE: %p\n", bh);
	if(!bh) {
		return -TE_NOSPC;
	}
	bh->flags = bh->head = bh->tail = 0;
	mutex_init(&bh->readlock);
	mutex_init(&bh->writelock);
	bh->nbits = nbits;
	objid_t oid;
	oid = twz_name_resolve(NULL, "libtwz.so.0", NAME_RESOLVER_DEFAULT);
	if(oid == 0) {
		return -TE_FAILURE;
	}
	struct object lt;
	twz_object_open(&lt, oid, FE_READ);
	ssize_t fe = twz_object_fot_add(obj, oid, FE_READ | FE_EXEC);
	if(fe < 0) {
		return fe;
	}
	uintptr_t r, w, n;
	if(!twz_obj_get_metavar_strname(&lt, "sym:bstream_read", &r)) {
		//return -TE_FAILURE;
	}
	if(!twz_obj_get_metavar_strname(&lt, "sym:bstream_write", &w)) {
		//return -TE_FAILURE;
	}
	if(!twz_obj_get_metavar_strname(&lt, "sym:bstream_notify_prepare", &n)) {
		//return -TE_FAILURE;
	}
	struct twzio_header tioh = {
		.read  = twz_make_canon_ptr(fe+1, r),
		.write = twz_make_canon_ptr(fe+1, w),
	};
	twzio_init(obj, &tioh);
	if(notify_init(obj, (struct notify_svar *)
				((char *)OBJ_NULLPAGE_SIZE + (1ul << nbits)),
				1024, twz_make_canon_ptr(fe+1, n)) < 0) {
		/* TODO: cleanup void */
		return -TE_NOSPC;
	}
	bh->rwid = notify_insert(obj, &bh->rwait);
	bh->wwid = notify_insert(obj, &bh->wwait);
	return 0;
}

