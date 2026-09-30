#ifndef MEMKIT_INTERNAL_H
#define MEMKIT_INTERNAL_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ASan integration: freed or not-yet-carved memory is poisoned so ASan
 * reports use-after-free and overflow inside our own pools, which it could
 * otherwise never see (to ASan, a pool page is one big live malloc). */
#if defined(__SANITIZE_ADDRESS__)
#  define MK_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define MK_ASAN 1
#  endif
#endif
#ifdef MK_ASAN
#  include <sanitizer/asan_interface.h>
#  define MK_POISON(p, n)   ASAN_POISON_MEMORY_REGION((p), (n))
#  define MK_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#  define MK_POISON(p, n)   ((void)(p), (void)(n))
#  define MK_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

#define MK_LIKELY(x)   __builtin_expect(!!(x), 1)
#define MK_UNLIKELY(x) __builtin_expect(!!(x), 0)

static inline int mk_is_pow2(size_t x) { return x && !(x & (x - 1)); }
static inline uintptr_t mk_align_up(uintptr_t p, size_t a) { return (p + (a - 1)) & ~(uintptr_t)(a - 1); }

__attribute__((noreturn, cold)) static inline void mk_panic(const char *msg, const void *p) {
    fprintf(stderr, "memkit: fatal: %s (ptr=%p)\n", msg, p);
    abort();
}
#endif
