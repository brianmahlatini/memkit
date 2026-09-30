#define _GNU_SOURCE
#include "memkit/arena.h"
#include "memkit/heap.h"
#include "memkit/pool.h"
#include "../src/internal.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_run = 0, g_failed = 0;
#define CHECK(c) do { g_run++; if (!(c)) { g_failed++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define RUN(t) do { fprintf(stderr, "[ RUN  ] %s\n", #t); t(); } while (0)

/* Runs fn in a child; returns 1 if the child died abnormally (abort/ASan). */
static int dies(void (*fn)(void)) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        fn();
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) != 0);
}

/* ------------------------------------------------------------ arena ---- */
static void test_arena_alignment_and_growth(void) {
    mk_arena a;
    mk_arena_init(&a, 128);
    for (size_t align = 1; align <= 4096; align <<= 1) {
        void *p = mk_arena_alloc(&a, 3, align);
        CHECK(p && ((uintptr_t)p % align) == 0);
    }
    CHECK(mk_arena_alloc(&a, 8, 3) == NULL);            /* non-power-of-two */
    CHECK(mk_arena_alloc(&a, SIZE_MAX, 8) == NULL);     /* overflow */
    CHECK(mk_arena_calloc(&a, SIZE_MAX / 2, 4, 8) == NULL);
    unsigned char *big = mk_arena_alloc(&a, 1 << 20, 16);
    CHECK(big != NULL);
    memset(big, 0xAB, 1 << 20);
    int *z = mk_arena_calloc(&a, 100, sizeof(int), _Alignof(int));
    int zero = 1;
    for (int i = 0; i < 100; i++) zero &= z[i] == 0;
    CHECK(zero);
    CHECK(strcmp(mk_arena_strndup(&a, "hello world", 5), "hello") == 0);
    mk_arena_destroy(&a);
    CHECK(a.head == NULL && a.reserved == 0);
}

static void test_arena_marks(void) {
    mk_arena a;
    mk_arena_init(&a, 256);
    char *keep = mk_arena_strndup(&a, "persistent", 64);
    mk_arena_mark m = mk_arena_save(&a);
    for (int i = 0; i < 1000; i++) mk_arena_alloc(&a, 100, 8);   /* spills into new chunks */
    size_t grown = a.reserved;
    mk_arena_restore(&a, m);
    CHECK(a.reserved < grown);
    CHECK(strcmp(keep, "persistent") == 0);
    void *again = mk_arena_alloc(&a, 1, 1);
    CHECK((char *)again == keep + strlen("persistent") + 1);    /* bump resumed at mark */
    for (int i = 0; i < 1000; i++) mk_arena_alloc(&a, 100, 8);
    size_t before_reset = a.reserved;
    mk_arena_reset(&a);
    CHECK(a.head != NULL);                                        /* one chunk kept for reuse */
    CHECK(a.reserved > 0 && a.reserved < before_reset);
    mk_arena_destroy(&a);
}

/* ------------------------------------------------------------- pool ---- */
static void test_pool_reuse_and_lazy_pages(void) {
    mk_pool p;
    CHECK(mk_pool_init(&p, 24, 64) == 0);
    CHECK(p.obj_size == 32);
    CHECK(p.capacity == 0);                               /* nothing allocated yet */
    void *objs[200];
    for (int i = 0; i < 200; i++) {
        objs[i] = mk_pool_alloc(&p);
        CHECK(((uintptr_t)objs[i] & 15) == 0);
        memset(objs[i], i, 24);
    }
    CHECK(p.live == 200 && p.capacity == 256);
    int intact = 1;
    for (int i = 0; i < 200; i++) intact &= ((unsigned char *)objs[i])[23] == (unsigned char)i;
    CHECK(intact);
    void *last = objs[199];
    mk_pool_free(&p, last);
    CHECK(mk_pool_alloc(&p) == last);                     /* LIFO reuse: cache-hot */
    for (int i = 0; i < 200; i++) mk_pool_free(&p, objs[i]);
    CHECK(p.live == 0);
    mk_pool_free(&p, NULL);
    mk_pool_destroy(&p);

    mk_pool bad;                                          /* separate object: init zeroes its target */
    CHECK(mk_pool_init(&bad, 0, 1) == -1);
}

static mk_pool g_dp;
static void pool_double_free(void) {
    mk_pool_init(&g_dp, 32, 8);
    void *o = mk_pool_alloc(&g_dp);
    mk_pool_free(&g_dp, o);
    mk_pool_free(&g_dp, o);
}
static void pool_use_after_free(void) {
    mk_pool_init(&g_dp, 32, 8);
    volatile char *o = mk_pool_alloc(&g_dp);
    mk_pool_free(&g_dp, (void *)o);
    o[8] = 1;
}
static void pool_foreign_free(void) {
    mk_pool_init(&g_dp, 32, 8);
    (void)mk_pool_alloc(&g_dp);
    int x;
    mk_pool_free(&g_dp, &x);
}

static void test_pool_error_detection(void) {
#if defined(MEMKIT_HARDEN) || defined(MK_ASAN)
    CHECK(dies(pool_double_free));
#endif
#ifdef MEMKIT_HARDEN
    CHECK(dies(pool_foreign_free));
#endif
#ifdef MK_ASAN
    CHECK(dies(pool_use_after_free));
#endif
    (void)pool_double_free; (void)pool_foreign_free; (void)pool_use_after_free;
}

/* ------------------------------------------------------------- heap ---- */
static uint64_t rng_state = 0x243F6A8885A308D3ULL;
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }

static void test_heap_size_classes(void) {
    mk_heap h;
    CHECK(mk_heap_init(&h, 64u << 20) == 0);
    for (size_t sz = 1; sz <= MK_HEAP_MAX_SMALL; sz++) {
        void *p = mk_heap_alloc(&h, sz);
        size_t u = mk_heap_usable_size(&h, p);
        if (!(p && ((uintptr_t)p & 15) == 0 && u >= sz && (sz <= 128 ? u - sz < 16 : u <= sz + sz / 4 + 16))) {
            CHECK(0 && "size class bound violated");
            fprintf(stderr, "    size=%zu usable=%zu\n", sz, u);
            break;
        }
        mk_heap_free(&h, p);
    }
    void *big = mk_heap_alloc(&h, 1 << 20);
    CHECK(big && mk_heap_usable_size(&h, big) >= (1 << 20));
    CHECK(mk_heap_stats_get(&h).large_bytes >= (1 << 20));
    mk_heap_free(&h, big);
    CHECK(mk_heap_stats_get(&h).large_bytes == 0);
    mk_heap_destroy(&h);
}

/* Randomised stress against a shadow table: every live block is filled with
 * a pattern derived from its slot and verified before it is freed, so any
 * overlap between two live allocations is caught. */
static void test_heap_random_stress(void) {
    mk_heap h;
    CHECK(mk_heap_init(&h, 256u << 20) == 0);
    enum { SLOTS = 4096, STEPS = 400000 };
    static unsigned char *ptr[SLOTS];
    static size_t len[SLOTS];
    int corrupt = 0;
    for (int step = 0; step < STEPS; step++) {
        size_t i = rnd() % SLOTS;
        if (ptr[i]) {
            for (size_t k = 0; k < len[i]; k++) corrupt |= ptr[i][k] != (unsigned char)(i + k);
            if (rnd() % 4 == 0) {
                size_t nl = 1 + rnd() % 3000;
                unsigned char *np = mk_heap_realloc(&h, ptr[i], nl);
                size_t keep = nl < len[i] ? nl : len[i];
                for (size_t k = 0; k < keep; k++) corrupt |= np[k] != (unsigned char)(i + k);
                ptr[i] = np; len[i] = nl;
                for (size_t k = 0; k < nl; k++) np[k] = (unsigned char)(i + k);
            } else {
                mk_heap_free(&h, ptr[i]);
                ptr[i] = NULL;
            }
        } else {
            size_t r = rnd() % 100;
            len[i] = r < 90 ? 1 + rnd() % 256 : r < 99 ? 1 + rnd() % MK_HEAP_MAX_SMALL : 1 + rnd() % 100000;
            ptr[i] = mk_heap_alloc(&h, len[i]);
            if (!ptr[i]) { corrupt = 1; break; }
            for (size_t k = 0; k < len[i]; k++) ptr[i][k] = (unsigned char)(i + k);
        }
    }
    CHECK(!corrupt);
    size_t peak = mk_heap_stats_get(&h).committed_slabs;
    for (size_t i = 0; i < SLOTS; i++) mk_heap_free(&h, ptr[i]);
    mk_heap_stats st = mk_heap_stats_get(&h);
    CHECK(st.large_bytes == 0);
    CHECK(st.committed_slabs <= MK_HEAP_NCLASSES);        /* at most one cached empty slab per class */
    CHECK(st.committed_slabs < peak || peak <= MK_HEAP_NCLASSES);
    mk_heap_destroy(&h);
}

static mk_heap g_dh;
static void heap_foreign_free(void) { mk_heap_init(&g_dh, 1 << 20); int x[8]; mk_heap_free(&g_dh, &x[4]); }
static void heap_interior_free(void) {
    mk_heap_init(&g_dh, 1 << 20);
    char *p = mk_heap_alloc(&g_dh, 64);
    mk_heap_free(&g_dh, p + 8);
}
static void heap_cross_heap_free(void) {
    mk_heap other;
    mk_heap_init(&g_dh, 1 << 20);
    mk_heap_init(&other, 1 << 20);
    mk_heap_free(&other, mk_heap_alloc(&g_dh, 32));
}

static void test_heap_invalid_frees_abort(void) {
    CHECK(dies(heap_foreign_free));
#ifdef MEMKIT_HARDEN
    CHECK(dies(heap_interior_free));
#endif
    (void)heap_interior_free;
    CHECK(dies(heap_cross_heap_free));
}

int main(void) {
    RUN(test_arena_alignment_and_growth);
    RUN(test_arena_marks);
    RUN(test_pool_reuse_and_lazy_pages);
    RUN(test_pool_error_detection);
    RUN(test_heap_size_classes);
    RUN(test_heap_random_stress);
    RUN(test_heap_invalid_frees_abort);
    fprintf(stderr, "%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
