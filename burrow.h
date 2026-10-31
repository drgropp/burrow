#ifndef BURROW_H
#define BURROW_H

#include <stddef.h>

/* Burrow: a fast general-purpose memory allocator.
   Thread-safe: each thread gets its own heap, so most calls take no lock. */

void *balloc(size_t n);
void *bcalloc(size_t count, size_t size);
void *brealloc(void *ptr, size_t n);
void bfree(void *ptr);

/* Stats, for tests and benchmarks. */
size_t burrow_committed(void);   /* bytes of the main region the OS has given us */
size_t burrow_usable(void *ptr); /* how many bytes this block can actually hold */

#endif
