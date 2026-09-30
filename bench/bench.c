/* Compares memkit allocators against the system malloc on common patterns. */
#define _GNU_SOURCE
#include "memkit/arena.h"
#include "memkit/heap.h"
#include "memkit/pool.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
static volatile uintptr_t sink;

static long rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, rss = 0;
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; fclose(f); }
    return rss * 4;
}
enum { N = 5000000, LIVE = 10000 };

static uint64_t s = 88172645463325252ULL;
static inline uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

static void report(const char *name, double t_sys, double t_mk) {
    printf("%-38s malloc %7.1f ns/op   memkit %6.1f ns/op   %5.1fx\n",
           name, t_sys * 1e9 / N, t_mk * 1e9 / N, t_sys / t_mk);
}

int main(void) {
    static void *slots[LIVE];
    static uint16_t sizes[N];
    for (int i = 0; i < N; i++) sizes[i] = (uint16_t)(8 + rnd() % 504);

    /* 1. Request-scoped: many small allocations, freed all at once. */
    double t0 = now();
    for (int r = 0; r < N / 1000; r++) {
        void *batch[1000];
        for (int i = 0; i < 1000; i++) { batch[i] = malloc(sizes[r * 1000 + i]); sink ^= (uintptr_t)batch[i]; }
        for (int i = 0; i < 1000; i++) free(batch[i]);
    }
    double t_sys = now() - t0;
    mk_arena a; mk_arena_init(&a, 1 << 20);
    t0 = now();
    for (int r = 0; r < N / 1000; r++) {
        for (int i = 0; i < 1000; i++) sink ^= (uintptr_t)mk_arena_alloc(&a, sizes[r * 1000 + i], 16);
        mk_arena_reset(&a);
    }
    report("arena: 1000 allocs then bulk reset", t_sys, now() - t0);
    mk_arena_destroy(&a);

    /* 2. Fixed-size churn with a live working set (e.g. connection objects). */
    memset(slots, 0, sizeof slots);
    t0 = now();
    for (int i = 0; i < N; i++) { size_t k = rnd() % LIVE; free(slots[k]); slots[k] = malloc(64); }
    for (int i = 0; i < LIVE; i++) { free(slots[i]); slots[i] = NULL; }
    t_sys = now() - t0;
    mk_pool p; mk_pool_init(&p, 64, 4096);
    t0 = now();
    for (int i = 0; i < N; i++) { size_t k = rnd() % LIVE; mk_pool_free(&p, slots[k]); slots[k] = mk_pool_alloc(&p); }
    report("pool: 64B churn, 10k live", t_sys, now() - t0);
    mk_pool_destroy(&p);
    memset(slots, 0, sizeof slots);

    /* 3. Mixed sizes 8..512 B with a live working set. */
    t0 = now();
    for (int i = 0; i < N; i++) { size_t k = rnd() % LIVE; free(slots[k]); slots[k] = malloc(sizes[i]); }
    for (int i = 0; i < LIVE; i++) { free(slots[i]); slots[i] = NULL; }
    t_sys = now() - t0;
    mk_heap h; mk_heap_init(&h, 0);
    t0 = now();
    for (int i = 0; i < N; i++) { size_t k = rnd() % LIVE; mk_heap_free(&h, slots[k]); slots[k] = mk_heap_alloc(&h, sizes[i]); }
    report("heap: mixed 8-512B churn, 10k live", t_sys, now() - t0);
    mk_heap_destroy(&h);

    /* 4. Memory density: resident bytes per 24-byte object, 2M objects. Every
     * object is written so both allocators fault in the pages they use. */
    enum { M = 2000000 };
    static void *objs[M];
    long before = rss_kb();
    for (int i = 0; i < M; i++) { objs[i] = malloc(24); memset(objs[i], 1, 24); }
    double sys_b = (double)(rss_kb() - before) * 1024.0 / M;
    for (int i = 0; i < M; i++) free(objs[i]);
    mk_heap_init(&h, 0);
    before = rss_kb();
    for (int i = 0; i < M; i++) { objs[i] = mk_heap_alloc(&h, 24); memset(objs[i], 1, 24); }
    double mk_b = (double)(rss_kb() - before) * 1024.0 / M;
    printf("%-38s malloc %7.1f B/obj    memkit %6.1f B/obj   %4.0f%% less memory\n",
           "density: 2M x 24-byte objects", sys_b, mk_b, 100.0 * (1.0 - mk_b / sys_b));
    mk_heap_destroy(&h);
    return 0;
}
