# Burrow

A fast, thread-safe memory allocator in C, written from scratch.

Burrow gets its memory straight from the operating system (`VirtualAlloc` on
Windows, `mmap` on Linux and macOS) and never calls `malloc`. It's a drop-in
style replacement for `malloc`, `calloc`, `realloc` and `free`:

```c
#include "burrow.h"

int *numbers = balloc(100 * sizeof(int));
numbers = brealloc(numbers, 200 * sizeof(int));
bfree(numbers);
```

> **Status: experimental (v0.5.0).** Burrow passes its full test suite on
> Windows and Linux, including under AddressSanitizer, UndefinedBehaviorSanitizer
> and ThreadSanitizer, but it hasn't been used in a real program yet.

## Results

Measured on a Windows laptop (CPU: AMD A8-7410 APU), built with
`gcc -O3 -march=native`, against the Windows C runtime's `malloc` and
Microsoft's [mimalloc](https://github.com/microsoft/mimalloc).

**Speed** (median of 15 runs per operation; "tie" = within measurement noise)

| Pattern | Burrow | malloc | mimalloc | Verdict |
| --- | --- | --- | --- | --- |
| Same size, freed in order | 15.2 ns | 272.2 ns | 11.2 ns | tie |
| Same size, freed in reverse | 18.0 ns | 324.3 ns | 17.5 ns | tie |
| Random sizes and order | 34.9 ns | 133.7 ns | 31.4 ns | tie |
| Zeroed (calloc) | 21.8 ns | 151.2 ns | 33.1 ns | Burrow 1.52x |
| Growing arrays (realloc) | 21.9 ns | 960.9 ns | 550.5 ns | Burrow 25x |
| Random mix, 4 threads | 19.9 ns | 62.5 ns | 17.2 ns | tie |
| Random mix, 8 threads | 12.6 ns | 51.3 ns | 10.1 ns | mimalloc 1.24x |

**Memory** (peak memory as a % of what the program actually needed; 100% is perfect)

| Pattern | Burrow | malloc | mimalloc |
| --- | --- | --- | --- |
| 300k small objects | 137% | 147% | 135% |
| 3k medium objects (1–64 KB) | 107% | 100% | 108% |
| Sizes shift midway | 128% | 140% | 118% |
| Made on one thread, freed on another | 180% | 152% | 135% |

Results depend on hardware. On Linux, glibc's `malloc` is much faster than the
Windows C runtime's, so Burrow's lead over `malloc` there is about 1.5–2x on
most patterns (7x on realloc). Run the benchmarks yourself, below.

## How it works

- **Size classes.** Requests round up to a class: 16 bytes apart up to 1 KB,
  then four classes per doubling up to 128 KB. Larger requests get their own
  OS mapping.
- **Pages, no per-block headers.** Memory is split into 64 KB slices. Each
  page holds blocks of one size class and stores that size once, in its
  header. A slice map finds any block's page with one lookup, so a 32-byte
  request uses exactly 32 bytes.
- **A heap per thread.** Each thread allocates and frees from its own pages,
  so the common path takes no locks. A block freed on another thread is sent
  back to its owner through a lock-free list.
- **Page recycling.** When every block in a page is free, the page goes to a
  shared pool that any size class can reuse. Extra empty pages hand their
  memory back to the OS.
- **Fast realloc.** Growing blocks move to a "grow chunk" once, then keep
  growing in place without copying.
- **Fast calloc.** Memory fresh from the OS is already zero, so Burrow only
  clears memory that's been used before.
- **Thread recycling.** When a thread exits, its heap (and everything in it)
  passes to the next new thread instead of leaking.

## Building

Burrow is two files: `burrow.h` and `burrow.c`. Add them to your project.
It needs a C11 compiler; it's tested with GCC (MinGW on Windows, and Linux).

### Tests

```
gcc -std=c11 -Wall -Wextra -O2 burrow.c tests.c -o tests
./tests
```

On Linux, add `-pthread`.

### Debug build

```
gcc -std=c11 -O2 -DBURROW_DEBUG burrow.c tests.c -o tests_debug
```

Release builds catch back-to-back double frees and pointers that didn't come
from Burrow. `-DBURROW_DEBUG` adds full double-free detection and checks that
every freed pointer is a real block start. It's slower, so use it while
developing.

### Benchmarks

```
gcc -std=c11 -O3 -march=native burrow.c bench.c -o bench -lpsapi
./bench        # speed
./bench mem    # memory use
```

To include mimalloc:

```
git clone https://github.com/microsoft/mimalloc.git
gcc -std=gnu11 -O3 -march=native -DNDEBUG -I mimalloc/include -c mimalloc/src/static.c -o mimalloc.o
gcc -std=c11 -O3 -march=native -DUSE_MIMALLOC -I mimalloc/include burrow.c bench.c mimalloc.o -o bench -lpsapi -ladvapi32 -lbcrypt
```

On Linux, use `-pthread` instead of the `-l` libraries.

### Proof it never calls malloc

```
gcc -std=c11 -O2 -c burrow.c -o burrow.o
nm -u burrow.o
```

This lists every outside function Burrow uses. The only memory functions are
the OS calls (`VirtualAlloc`/`VirtualFree` or `mmap`/`munmap`).

## API

| Function | Like | Notes |
| --- | --- | --- |
| `void *balloc(size_t n)` | `malloc` | Returns `NULL` for size 0 |
| `void *bcalloc(size_t count, size_t size)` | `calloc` | Catches `count * size` overflow |
| `void *brealloc(void *p, size_t n)` | `realloc` | `brealloc(NULL, n)` allocates; `brealloc(p, 0)` frees |
| `void bfree(void *p)` | `free` | `bfree(NULL)` does nothing |
| `size_t burrow_usable(void *p)` | | How many bytes the block can actually hold |
| `size_t burrow_committed(void)` | | Memory the OS has given Burrow's main region |

## Known limitations

- Release builds only catch the most common double frees; use `-DBURROW_DEBUG`
  for full checks.
- A page returns to the pool only when every block in it is free, so a few
  long-lived blocks can keep a page from being reused.
- Blocks freed on another thread wait in their owner's list until the owner
  needs memory, which costs memory in producer/consumer patterns.
- More than 1,024 threads alive at the same time fall back to one shared heap
  that isn't safe.
- Tested on x86-64 with GCC only.

## Files

| File | What it is |
| --- | --- |
| `burrow.h`, `burrow.c` | The allocator |
| `tests.c` | Correctness tests, including multithreaded ones |
| `safety.c` | Shows Burrow catching a double free and a bad pointer |
| `bench.c` | Speed and memory benchmarks |
| `threads.h` | Small thread wrapper used by the tests and benchmarks |

## How it was built

Burrow started as a hand-written bump allocator for learning how `malloc`
works. Later versions were
developed with AI assistance, then tested and benchmarked on real hardware.

## License

MIT. See [LICENSE](LICENSE).
