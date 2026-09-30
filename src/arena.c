#include "memkit/arena.h"
#include "internal.h"
#include <string.h>

struct mk_chunk {
    mk_chunk *prev;
    size_t    cap;
    size_t    used;
    _Alignas(16) unsigned char data[];
};

void mk_arena_init(mk_arena *a, size_t min_chunk) {
    a->head = NULL;
    a->min_chunk = min_chunk ? min_chunk : 64 * 1024;
    a->reserved = 0;
}

static mk_chunk *chunk_new(mk_arena *a, size_t need) {
    size_t cap = a->head ? a->head->cap * 2 : a->min_chunk;   /* geometric growth */
    if (cap < need) cap = need;
    if (cap > SIZE_MAX - sizeof(mk_chunk)) return NULL;
    mk_chunk *c = malloc(sizeof(mk_chunk) + cap);
    if (!c) return NULL;
    c->prev = a->head;
    c->cap = cap;
    c->used = 0;
    MK_POISON(c->data, cap);
    a->head = c;
    a->reserved += cap;
    return c;
}

static void *bump(mk_chunk *c, size_t size, size_t align) {
    uintptr_t base = (uintptr_t)c->data;
    uintptr_t p = mk_align_up(base + c->used, align);
    size_t off = (size_t)(p - base);
    if (off > c->cap || size > c->cap - off) return NULL;
    c->used = off + size;
    MK_UNPOISON((void *)p, size);
    return (void *)p;
}

void *mk_arena_alloc(mk_arena *a, size_t size, size_t align) {
    if (!mk_is_pow2(align)) return NULL;
    if (size == 0) size = 1;
    if (MK_LIKELY(a->head != NULL)) {
        void *p = bump(a->head, size, align);
        if (MK_LIKELY(p != NULL)) return p;
    }
    if (size > SIZE_MAX - align) return NULL;
    if (!chunk_new(a, size + align)) return NULL;
    return bump(a->head, size, align);
}

void *mk_arena_calloc(mk_arena *a, size_t n, size_t size, size_t align) {
    if (size && n > SIZE_MAX / size) return NULL;
    void *p = mk_arena_alloc(a, n * size, align);
    if (p) memset(p, 0, n * size);
    return p;
}

char *mk_arena_strndup(mk_arena *a, const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *d = mk_arena_alloc(a, len + 1, 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

mk_arena_mark mk_arena_save(const mk_arena *a) {
    mk_arena_mark m = { a->head, a->head ? a->head->used : 0 };
    return m;
}

void mk_arena_restore(mk_arena *a, mk_arena_mark m) {
    while (a->head && a->head != m.chunk) {
        mk_chunk *prev = a->head->prev;
        a->reserved -= a->head->cap;
        MK_UNPOISON(a->head->data, a->head->cap);
        free(a->head);
        a->head = prev;
    }
    if (a->head) {
        MK_POISON(a->head->data + m.used, a->head->cap - m.used);
        a->head->used = m.used;
    }
}

void mk_arena_reset(mk_arena *a) {
    if (!a->head) return;
    /* Keep the newest (largest) chunk so a steady-state workload stops
     * touching the system allocator entirely after warm-up. */
    mk_chunk *keep = a->head;
    mk_chunk *c = keep->prev;
    while (c) {
        mk_chunk *p = c->prev;
        a->reserved -= c->cap;
        MK_UNPOISON(c->data, c->cap);
        free(c);
        c = p;
    }
    keep->prev = NULL;
    keep->used = 0;
    MK_POISON(keep->data, keep->cap);
}

void mk_arena_destroy(mk_arena *a) {
    mk_arena_restore(a, (mk_arena_mark){ NULL, 0 });
    a->reserved = 0;
}
