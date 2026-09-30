/* pool.h - fixed-size object pool with an intrusive free list.
 *
 * O(1) alloc and free with no per-object header. Pages are carved lazily, so
 * creating a pool touches no memory and a page's untouched tail never faults
 * in. With MEMKIT_HARDEN, frees are checked for double-free and foreign
 * pointers. */
#ifndef MEMKIT_POOL_H
#define MEMKIT_POOL_H
#include <stddef.h>

typedef struct mk_pool_page mk_pool_page;

typedef struct {
    size_t        obj_size;      /* rounded to 16-byte multiple, >= 16 */
    size_t        per_page;
    void         *free_list;
    mk_pool_page *pages;
    unsigned char *bump, *bump_end;  /* uncarved region of newest page */
    size_t        live;
    size_t        capacity;      /* objects across all pages */
} mk_pool;

int    mk_pool_init(mk_pool *p, size_t obj_size, size_t objs_per_page); /* 0 ok, -1 bad args. Overwrites *p: never call on a live pool (destroy first). */
void  *mk_pool_alloc(mk_pool *p);                                      /* NULL on OOM */
void   mk_pool_free(mk_pool *p, void *obj);                             /* NULL is a no-op */
void   mk_pool_destroy(mk_pool *p);
#endif
