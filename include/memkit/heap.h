/* heap.h - general-purpose size-class allocator (malloc/free replacement).
 *
 * Small requests (<= MK_HEAP_MAX_SMALL) are served from 64 KiB slabs, each
 * dedicated to one size class, carved out of a single reserved virtual
 * address range. free() finds a small object's slab by masking the pointer
 * and tells small from large with a range check, so there are no per-object
 * headers and no lookups. Large requests map pages directly.
 *
 * A heap is NOT thread-safe: use one heap per thread (the thread-caching
 * model of tcmalloc/mimalloc without the cross-thread free path). */
#ifndef MEMKIT_HEAP_H
#define MEMKIT_HEAP_H
#include <stddef.h>
#include <stdint.h>

#define MK_HEAP_SLAB_SIZE  ((size_t)64 * 1024)
#define MK_HEAP_MAX_SMALL  ((size_t)8 * 1024)
#define MK_HEAP_NCLASSES   32

typedef struct mk_slab mk_slab;

typedef struct {
    mk_slab *partial;     /* slabs with free space; head is allocated from */
    size_t   size;        /* object size of this class */
    size_t   empty_cached;
} mk_size_class;

typedef struct {
    unsigned char *base;        /* reserved VA range for slabs */
    size_t         reserve;
    size_t         next_fresh;  /* slabs never yet committed start here */
    mk_slab       *recycled;    /* decommitted slabs available for reuse */
    mk_size_class  classes[MK_HEAP_NCLASSES];
    uint8_t        class_of[MK_HEAP_MAX_SMALL / 16 + 1];  /* (size+15)/16 -> class */
    size_t         committed_slabs;
    size_t         large_bytes;
} mk_heap;

typedef struct {
    size_t committed_slabs, small_bytes_committed, large_bytes;
} mk_heap_stats;

/* reserve_bytes: size of the VA range for slabs (0 = 4 GiB). Reserving
 * address space is free; memory is committed per slab on demand. */
int     mk_heap_init(mk_heap *h, size_t reserve_bytes);
void   *mk_heap_alloc(mk_heap *h, size_t size);            /* 16-byte aligned; NULL on OOM */
void   *mk_heap_calloc(mk_heap *h, size_t n, size_t size);
void   *mk_heap_realloc(mk_heap *h, void *p, size_t size);
void    mk_heap_free(mk_heap *h, void *p);
size_t  mk_heap_usable_size(const mk_heap *h, const void *p);
mk_heap_stats mk_heap_stats_get(const mk_heap *h);
void    mk_heap_destroy(mk_heap *h);
#endif
