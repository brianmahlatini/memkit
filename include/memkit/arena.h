/* arena.h - bump-pointer arena allocator.
 *
 * Allocation is a pointer bump (a few instructions); there is no individual
 * free. Memory is released in bulk with mk_arena_reset() or rolled back to a
 * saved mark, which suits request-scoped or frame-scoped lifetimes: parse a
 * request into an arena, answer it, reset. */
#ifndef MEMKIT_ARENA_H
#define MEMKIT_ARENA_H
#include <stddef.h>

typedef struct mk_chunk mk_chunk;

typedef struct {
    mk_chunk *head;          /* newest chunk */
    size_t    min_chunk;     /* minimum chunk payload size */
    size_t    reserved;      /* total bytes held from the system */
} mk_arena;

typedef struct { mk_chunk *chunk; size_t used; } mk_arena_mark;

void          mk_arena_init(mk_arena *a, size_t min_chunk);
/* align must be a power of two. Returns NULL on OOM, overflow or bad align. */
void         *mk_arena_alloc(mk_arena *a, size_t size, size_t align);
void         *mk_arena_calloc(mk_arena *a, size_t n, size_t size, size_t align);
char         *mk_arena_strndup(mk_arena *a, const char *s, size_t n);
mk_arena_mark mk_arena_save(const mk_arena *a);
void          mk_arena_restore(mk_arena *a, mk_arena_mark m);  /* frees newer chunks */
void          mk_arena_reset(mk_arena *a);                     /* keeps one chunk for reuse */
void          mk_arena_destroy(mk_arena *a);
#endif
