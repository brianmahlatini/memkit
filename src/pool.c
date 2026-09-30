#include "memkit/pool.h"
#include "internal.h"
#include <string.h>

struct mk_pool_page {
    mk_pool_page *next;
    size_t        bytes;
    _Alignas(16) unsigned char data[];
};

#ifdef MEMKIT_HARDEN
/* A freed object stores [next][tag]. tag = next ^ secret ^ address, so a
 * stale or forged value is unlikely to validate by accident. */
static const uintptr_t k_free_secret = (uintptr_t)0x5bd1e9955bd1e995ULL;
static uintptr_t free_tag(void *obj, void *next) { return (uintptr_t)next ^ k_free_secret ^ (uintptr_t)obj; }
static int owns(const mk_pool *p, const void *obj) {
    for (const mk_pool_page *pg = p->pages; pg; pg = pg->next) {
        const unsigned char *o = obj;
        if (o >= pg->data && o < pg->data + pg->bytes)
            return ((size_t)(o - pg->data) % p->obj_size) == 0;
    }
    return 0;
}
#endif

int mk_pool_init(mk_pool *p, size_t obj_size, size_t objs_per_page) {
    memset(p, 0, sizeof *p);
    if (obj_size == 0 || obj_size > (SIZE_MAX >> 4)) return -1;
    p->obj_size = (size_t)mk_align_up(obj_size < 16 ? 16 : obj_size, 16);
    p->per_page = objs_per_page ? objs_per_page : 256;
    if (p->per_page > (SIZE_MAX - sizeof(mk_pool_page)) / p->obj_size) return -1;
    return 0;
}

static int add_page(mk_pool *p) {
    size_t bytes = p->obj_size * p->per_page;
    mk_pool_page *pg = malloc(sizeof *pg + bytes);
    if (!pg) return -1;
    pg->bytes = bytes;
    pg->next = p->pages;
    p->pages = pg;
    p->bump = pg->data;
    p->bump_end = pg->data + bytes;
    p->capacity += p->per_page;
    MK_POISON(pg->data, bytes);
    return 0;
}

void *mk_pool_alloc(mk_pool *p) {
    void *obj = p->free_list;
    if (MK_LIKELY(obj != NULL)) {
        MK_UNPOISON(obj, p->obj_size);
        void *next = *(void **)obj;
#ifdef MEMKIT_HARDEN
        if (((uintptr_t *)obj)[1] != free_tag(obj, next)) mk_panic("pool free list corrupted", obj);
        ((uintptr_t *)obj)[1] = 0;
#endif
        p->free_list = next;
    } else {
        if (p->bump == p->bump_end && add_page(p) != 0) return NULL;
        obj = p->bump;
        p->bump += p->obj_size;
        MK_UNPOISON(obj, p->obj_size);
    }
    p->live++;
    return obj;
}

void mk_pool_free(mk_pool *p, void *obj) {
    if (!obj) return;
#ifdef MEMKIT_HARDEN
    if (!owns(p, obj)) mk_panic("pool_free of pointer not owned by this pool", obj);
    MK_UNPOISON(obj, p->obj_size);              /* inspect it ourselves */
    if (((uintptr_t *)obj)[1] == free_tag(obj, *(void **)obj)) mk_panic("pool double free", obj);
    memset(obj, 0xDD, p->obj_size);             /* scribble: stale reads look obviously wrong */
    ((uintptr_t *)obj)[1] = free_tag(obj, p->free_list);
#endif
    *(void **)obj = p->free_list;
    p->free_list = obj;
    p->live--;
    MK_POISON(obj, p->obj_size);
}

void mk_pool_destroy(mk_pool *p) {
    mk_pool_page *pg = p->pages;
    while (pg) {
        mk_pool_page *n = pg->next;
        MK_UNPOISON(pg->data, pg->bytes);
        free(pg);
        pg = n;
    }
    memset(p, 0, sizeof *p);
}
