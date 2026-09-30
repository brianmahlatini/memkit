#define _GNU_SOURCE
#include "memkit/heap.h"
#include "internal.h"
#include <string.h>
#include <sys/mman.h>

#define SLAB_MAGIC  0x534C4142u   /* "SLAB" */
#define LARGE_MAGIC 0x4C415247u   /* "LARG" */
#define SLAB_HDR    64            /* header bytes; objects start after, 16-aligned */
#define LARGE_HDR   16

struct mk_slab {
    uint32_t       magic;
    uint32_t       cls;
    mk_heap       *owner;
    mk_slab       *next, *prev;   /* partial-list links */
    void          *free_list;
    unsigned char *bump, *end;    /* uncarved tail */
    uint32_t       used, cap;
};
_Static_assert(sizeof(mk_slab) <= SLAB_HDR, "slab header too large");

typedef struct { uint32_t magic; uint32_t pad; size_t mapped; } large_hdr;
_Static_assert(sizeof(large_hdr) == LARGE_HDR, "large header must keep 16-byte alignment");

/* Size classes: 16..128 in steps of 16, then 4 geometric steps per power of
 * two up to 8 KiB. Worst-case internal fragmentation is 25% above 128 bytes
 * and at most 15 bytes below. */
static size_t build_classes(mk_heap *h) {
    size_t n = 0;
    for (size_t s = 16; s <= 128; s += 16) h->classes[n++].size = s;
    for (size_t base = 128; base < MK_HEAP_MAX_SMALL; base *= 2)
        for (size_t k = 1; k <= 4; k++) h->classes[n++].size = base + base / 4 * k;
    size_t c = 0;
    for (size_t i = 0; i <= MK_HEAP_MAX_SMALL / 16; i++) {
        size_t sz = i * 16;
        while (h->classes[c].size < sz) c++;
        h->class_of[i] = (uint8_t)c;
    }
    return n;
}

int mk_heap_init(mk_heap *h, size_t reserve_bytes) {
    memset(h, 0, sizeof *h);
    if (build_classes(h) != MK_HEAP_NCLASSES) return -1;
    size_t r = reserve_bytes ? reserve_bytes : ((size_t)4 << 30);
    r = (size_t)mk_align_up(r, MK_HEAP_SLAB_SIZE);
    /* Over-reserve by one slab so we can align the base to the slab size. */
    unsigned char *raw = mmap(NULL, r + MK_HEAP_SLAB_SIZE, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) return -1;
    unsigned char *base = (unsigned char *)mk_align_up((uintptr_t)raw, MK_HEAP_SLAB_SIZE);
    size_t head = (size_t)(base - raw), tail = MK_HEAP_SLAB_SIZE - head;
    if (head) munmap(raw, head);
    if (tail) munmap(base + r, tail);
    h->base = base;
    h->reserve = r;
    return 0;
}

static inline int in_slab_range(const mk_heap *h, const void *p) {
    return (uintptr_t)p - (uintptr_t)h->base < h->reserve;
}
static inline mk_slab *slab_of(const void *p) {
    return (mk_slab *)((uintptr_t)p & ~(uintptr_t)(MK_HEAP_SLAB_SIZE - 1));
}

static void list_remove(mk_size_class *c, mk_slab *s) {
    if (s->prev) s->prev->next = s->next; else c->partial = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = NULL;
}
static void list_push(mk_size_class *c, mk_slab *s) {
    s->prev = NULL;
    s->next = c->partial;
    if (c->partial) c->partial->prev = s;
    c->partial = s;
}

static mk_slab *slab_acquire(mk_heap *h, uint32_t cls) {
    mk_slab *s;
    if (h->recycled) {
        s = h->recycled;
        h->recycled = s->next;
        MK_UNPOISON(s, MK_HEAP_SLAB_SIZE);
    } else {
        if (h->next_fresh + MK_HEAP_SLAB_SIZE > h->reserve) return NULL;
        s = (mk_slab *)(h->base + h->next_fresh);
        if (mprotect(s, MK_HEAP_SLAB_SIZE, PROT_READ | PROT_WRITE) != 0) return NULL;
        h->next_fresh += MK_HEAP_SLAB_SIZE;
    }
    size_t osz = h->classes[cls].size;
    s->magic = SLAB_MAGIC;
    s->cls = cls;
    s->owner = h;
    s->next = s->prev = NULL;
    s->free_list = NULL;
    s->bump = (unsigned char *)s + SLAB_HDR;
    s->cap = (uint32_t)((MK_HEAP_SLAB_SIZE - SLAB_HDR) / osz);
    s->end = s->bump + (size_t)s->cap * osz;
    s->used = 0;
    MK_POISON(s->bump, MK_HEAP_SLAB_SIZE - SLAB_HDR);
    h->committed_slabs++;
    return s;
}

static void slab_release(mk_heap *h, mk_slab *s) {
    MK_UNPOISON(s, MK_HEAP_SLAB_SIZE);
    /* Return physical pages to the OS but keep the mapping for reuse. */
    madvise(s, MK_HEAP_SLAB_SIZE, MADV_DONTNEED);
    s->magic = 0;
    s->next = h->recycled;
    h->recycled = s;
    h->committed_slabs--;
    MK_POISON((unsigned char *)s + sizeof(mk_slab), MK_HEAP_SLAB_SIZE - sizeof(mk_slab));
}

static void *large_alloc(mk_heap *h, size_t size) {
    if (size > SIZE_MAX - LARGE_HDR - 4096) return NULL;
    size_t mapped = (size_t)mk_align_up(size + LARGE_HDR, 4096);
    void *m = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return NULL;
    large_hdr *hd = m;
    hd->magic = LARGE_MAGIC;
    hd->mapped = mapped;
    h->large_bytes += mapped;
    return (unsigned char *)m + LARGE_HDR;
}

void *mk_heap_alloc(mk_heap *h, size_t size) {
    if (size == 0) size = 1;
    if (MK_UNLIKELY(size > MK_HEAP_MAX_SMALL)) return large_alloc(h, size);
    uint32_t cls = h->class_of[(size + 15) >> 4];
    mk_size_class *c = &h->classes[cls];
    mk_slab *s = c->partial;
    if (MK_UNLIKELY(!s)) {
        if (!(s = slab_acquire(h, cls))) return NULL;
        list_push(c, s);
        c->empty_cached++;
    }
    if (s->used == 0) c->empty_cached--;
    void *obj;
    if (s->free_list) {
        obj = s->free_list;
        MK_UNPOISON(obj, c->size);
        s->free_list = *(void **)obj;
    } else {
        obj = s->bump;
        s->bump += c->size;
        MK_UNPOISON(obj, c->size);
    }
    if (++s->used == s->cap) list_remove(c, s);    /* full slabs leave the list */
    return obj;
}

void mk_heap_free(mk_heap *h, void *p) {
    if (!p) return;
    if (MK_UNLIKELY(!in_slab_range(h, p))) {
        large_hdr *hd = (large_hdr *)((unsigned char *)p - LARGE_HDR);
        if (((uintptr_t)p & 15) || hd->magic != LARGE_MAGIC) mk_panic("free of invalid pointer", p);
        hd->magic = 0;
        h->large_bytes -= hd->mapped;
        munmap(hd, hd->mapped);
        return;
    }
    mk_slab *s = slab_of(p);
    if (s->magic != SLAB_MAGIC || s->owner != h) mk_panic("free of pointer not owned by this heap", p);
    mk_size_class *c = &h->classes[s->cls];
#ifdef MEMKIT_HARDEN
    /* Interior-pointer check costs an integer division, so it is opt-in. */
    size_t off = (size_t)((unsigned char *)p - ((unsigned char *)s + SLAB_HDR));
    if ((unsigned char *)p >= s->bump || off % c->size) mk_panic("free of misaligned or uncarved pointer", p);
#endif
    *(void **)p = s->free_list;
    s->free_list = p;
    MK_POISON(p, c->size);
    if (s->used-- == s->cap) list_push(c, s);      /* was full: back on the list */
    if (s->used == 0) {
        /* Hysteresis: keep one empty slab per class so alloc/free ping-pong
         * at a slab boundary does not thrash mmap/madvise. */
        if (c->empty_cached >= 1) { list_remove(c, s); slab_release(h, s); }
        else c->empty_cached++;
    }
}

size_t mk_heap_usable_size(const mk_heap *h, const void *p) {
    if (!p) return 0;
    if (!in_slab_range(h, p)) {
        const large_hdr *hd = (const large_hdr *)((const unsigned char *)p - LARGE_HDR);
        return hd->mapped - LARGE_HDR;
    }
    return h->classes[slab_of(p)->cls].size;
}

void *mk_heap_calloc(mk_heap *h, size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    void *p = mk_heap_alloc(h, n * size);
    if (p && in_slab_range(h, p)) memset(p, 0, n * size);   /* fresh mmap is already zero */
    return p;
}

void *mk_heap_realloc(mk_heap *h, void *p, size_t size) {
    if (!p) return mk_heap_alloc(h, size);
    if (size == 0) { mk_heap_free(h, p); return NULL; }
    size_t have = mk_heap_usable_size(h, p);
    /* Shrink in place unless it would waste more than half the block. */
    if (size <= have && size > have / 2) return p;
    void *n = mk_heap_alloc(h, size);
    if (!n) return NULL;                        /* original block untouched, as with realloc */
    memcpy(n, p, size < have ? size : have);
    mk_heap_free(h, p);
    return n;
}

mk_heap_stats mk_heap_stats_get(const mk_heap *h) {
    mk_heap_stats st = { h->committed_slabs, h->committed_slabs * MK_HEAP_SLAB_SIZE, h->large_bytes };
    return st;
}

void mk_heap_destroy(mk_heap *h) {
    /* Large blocks still live are the caller's leak; small memory goes with
     * the reservation. */
    if (h->base) {
        MK_UNPOISON(h->base, h->next_fresh);
        munmap(h->base, h->reserve);
    }
    memset(h, 0, sizeof *h);
}
