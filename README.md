# memkit

Three allocators in C11, each built for a different allocation pattern, with AddressSanitizer integration so bugs *inside* pooled memory are still caught.

| Allocator | Pattern it wins on | Alloc | Free |
|---|---|---|---|
| `mk_arena` | Request- or frame-scoped data freed all at once | pointer bump | bulk reset or rollback to a mark |
| `mk_pool` | Many objects of one size with churn (connections, nodes, messages) | O(1) free-list pop | O(1) push |
| `mk_heap` | General `malloc`/`free` replacement, per thread | O(1) size-class slab | O(1), no per-object header |

```
arena: 1000 allocs then bulk reset     malloc    32.3 ns/op   memkit    2.4 ns/op    13.4x
pool: 64B churn, 10k live              malloc     9.8 ns/op   memkit    3.9 ns/op     2.5x
heap: mixed 8-512B churn, 10k live     malloc    18.8 ns/op   memkit   20.6 ns/op     0.9x
density: 2M x 24-byte objects          malloc    38.6 B/obj    memkit   32.0 B/obj     17% less memory
```
*glibc 2.39 malloc as baseline, GCC 13 `-O3`, single shared vCPU.* The general-purpose heap is at speed parity with glibc's tcache on random churn; its wins are density (no per-object header) and deterministic return of memory to the OS. The arena and pool are where the large speedups are, which is the usual lesson: the biggest allocator wins come from matching the allocator to the lifetime pattern, not from a faster `malloc`.

## Design

### Arena
Chunks grow geometrically (each double the last), so N bytes of allocation cost O(log N) system calls. `mk_arena_save`/`mk_arena_restore` roll back to a mark, freeing every chunk created after it: useful for "try to parse, roll back on failure". `mk_arena_reset` keeps the newest, largest chunk, so a steady-state request loop stops calling the system allocator entirely after warm-up. All size arithmetic is overflow-checked.

### Pool
No per-object header; a free object stores the free-list link in its own first word. Pages are carved lazily with a bump pointer, so creating a pool touches no memory and a page's unused tail never faults in. Reuse is LIFO, so the most recently freed (cache-hot) object is handed out next.

With `-DMEMKIT_HARDEN=ON`, each free object also stores a tag (`next ^ secret ^ address`). A second free of the same object is detected by the tag, frees of foreign pointers are rejected by an ownership check, freed memory is scribbled with `0xDD`, and a corrupted free list aborts on the next allocation instead of silently handing out an overlapping block.

### Heap
- **Size classes:** 16 to 128 bytes in steps of 16, then four geometric steps per power of two up to 8 KiB. That bounds internal waste at 15 bytes for small sizes and 25% above 128. Class lookup is a single table read.
- **Slabs:** Each 64 KiB slab holds objects of one class. All slabs are carved from one reserved, slab-aligned virtual address range (`mmap` with `PROT_NONE` and `MAP_NORESERVE`; memory is committed per slab on demand).
- **Header-free free():** A pointer's slab header is found by masking off the low 16 bits. Whether a pointer is small or large is a single range check against the reservation, so `free()` needs no lookup table and objects carry no header. Every free validates the slab magic and owning heap, so a foreign or cross-heap pointer aborts with a clear message instead of corrupting state.
- **Returning memory:** When a slab empties it is `madvise(MADV_DONTNEED)`'d and recycled. One empty slab per class is cached (hysteresis) so an alloc/free ping-pong at a slab boundary does not thrash system calls.
- **Large allocations** (> 8 KiB) are mapped directly with a 16-byte header, keeping 16-byte alignment.
- **Threading:** A heap is deliberately single-threaded: one heap per thread, the thread-caching model of tcmalloc and mimalloc minus the cross-thread free path. That keeps the fast path free of atomics.

### Sanitizer integration
To ASan, a pool page is one big live `malloc` block, so a use-after-free inside it would normally go unnoticed. memkit calls `ASAN_POISON_MEMORY_REGION` on every freed or not-yet-carved object and unpoisons on allocation, so ASan reports use-after-free and overflows inside arenas, pools, and slabs exactly as it would for `malloc`.

## Testing
- **Arena:** alignment for every power of two up to 4 KiB, overflow rejection, mark/rollback semantics, reset reuse
- **Pool:** lazy page carving, LIFO reuse, data integrity across 200 objects
- **Heap:** every size from 1 to 8192 checked against its class bound and alignment; a 400k-step randomised stress test mixing small, medium, and large allocations and reallocs, where every live block carries a slot-derived byte pattern verified before free (so any overlap between live blocks is caught); slab-release accounting after everything is freed
- **Death tests** (`fork` + expect abnormal exit): double free, foreign-pointer free, and interior-pointer free under `MEMKIT_HARDEN`; pool use-after-free under ASan; foreign and cross-heap frees always
- **CI:** GCC and Clang × {Release, hardened, ASan + UBSan}

## Build
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/memkit_bench
```

## Usage
```c
#include <memkit/arena.h>
mk_arena a;
mk_arena_init(&a, 64 * 1024);
request_t *req = mk_arena_alloc(&a, sizeof *req, _Alignof(request_t));
/* ... handle the request; every allocation for it lives in the arena ... */
mk_arena_reset(&a);   /* one call frees everything */
```

## Limitations
Linux/POSIX only (`mmap`, `madvise`). The heap has no cross-thread free and is not a drop-in `LD_PRELOAD` replacement for `malloc`. Adding a lock-free remote-free list per slab (as mimalloc does) is the natural next step.

## License
MIT
