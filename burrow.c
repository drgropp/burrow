#ifndef _WIN32
#define _DEFAULT_SOURCE /* for MAP_ANONYMOUS and madvise with -std=c11 */
#endif

#include "burrow.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* ======================================================================
   How Burrow is laid out (version 5)

   The region is one big reservation of address space, cut into 64 KB
   "slices". Memory is handed out in pages made of slices:

   - small pages  (1 slice,  64 KB): blocks of one size class, 16 B .. 8 KB
   - medium pages (16 slices, 1 MB): blocks of one size class, 8 KB .. 128 KB
   - grow chunks  (64 slices, 4 MB): blocks that brealloc is growing

   Each page keeps its size class in a small header at its start, so the
   blocks themselves carry no header at all. To find a block's page, Burrow
   looks up the block's slice in the slice map. Anything bigger than 128 KB
   gets its own OS mapping.

   When every block in a page is free, the page goes back to a shared pool,
   where any size class can reuse it. Pages that sit in the pool too long
   hand their memory back to the OS.
   ====================================================================== */

#define SLICE_SHIFT    16
#define SLICE          ((size_t)1 << SLICE_SHIFT)  /* 64 KB */
#define REGION_RESERVE ((size_t)1 << 32)           /* 4 GB of address space */
#define NUM_SLICES     (REGION_RESERVE / SLICE)    /* 65,536 */
#define COMMIT_STEP    ((size_t)1 << 20)           /* commit 1 MB at a time */

#define SMALL_PAGE_SLICES  1
#define MEDIUM_PAGE_SLICES 16
#define GROW_CHUNK_SLICES  64

/* Size classes: 16 bytes apart up to 1 KB, then four per doubling up to 128 KB. */
#define SMALL_MAX      1024
#define SMALL_CLASSES  (SMALL_MAX / 16)    /* 64 */
#define MEDIUM_MAX     ((size_t)128 << 10) /* 128 KB */
#define MEDIUM_CLASSES 28                  /* 1280 ... 131072 */
#define NUM_CLASSES    (SMALL_CLASSES + MEDIUM_CLASSES)
#define SMALL_PAGE_MAX 8192                /* classes up to here use small pages */

#define GROW_MAX ((size_t)1 << 20)         /* brealloc keeps growing in place up to 1 MB */

/* Empty pages kept ready in each pool before extra ones give memory back. */
#define KEEP_SMALL  64 /* 4 MB */
#define KEEP_MEDIUM 8  /* 8 MB */
#define KEEP_GROW   2  /* 8 MB */

#define BIG_MAGIC  ((size_t)0xB16B10C5B16B10C5u)
#define GROW_MAGIC ((size_t)0x6120B10C6120B10Cu)

enum { KIND_SMALL, KIND_MEDIUM, KIND_GROW };

struct FreeNode {
    struct FreeNode *next;
};

struct ThreadHeap;

/* Lives at the start of every page and grow chunk. */
struct Page {
    struct FreeNode *free;    /* freed blocks ready to reuse */
    char *bump;               /* next never-used block */
    char *limit;              /* end of the block area */
    char *start;              /* first block */
    struct Page *next, *prev; /* the heap's list of pages for this class */
    struct Page *pool_next;   /* link while sitting in a pool */
    struct ThreadHeap *heap;  /* owner */
    size_t block_size;
    uint32_t used;            /* live blocks (grow chunk: live grow blocks) */
    uint16_t cls;
    uint8_t kind;
    uint8_t in_list;          /* 1 if on the heap's list (not full) */
    uint8_t fresh;            /* 1 if never-used blocks are still zero */
    uint8_t purged;           /* 1 if its memory was handed back to the OS */
    uint32_t slices;
};
#define PAGE_HEADER ((sizeof(struct Page) + 63) & ~(size_t)63)

/* Grow blocks and big blocks do keep a small header, since their sizes vary. */
struct SizeHeader {
    alignas(max_align_t) size_t size;
    size_t magic;
};
#define SIZE_HEADER sizeof(struct SizeHeader)

/* ---------- heaps ---------- */

struct ThreadHeap {
    struct Page *pages[NUM_CLASSES];    /* pages with room, current one first */
    struct Page *grow;                  /* current grow chunk */
    _Atomic(struct FreeNode *) remote;  /* blocks other threads freed for us */
    uint32_t index;
    int in_use;
    int ever_used;
};

#define MAX_HEAPS 1024
static struct ThreadHeap heaps[MAX_HEAPS + 1]; /* the last one is a shared fallback */
#define FALLBACK (&heaps[MAX_HEAPS])
static _Thread_local struct ThreadHeap *my_heap = NULL;

/* ---------- shared state: region, slice map, page pools ---------- */

static char *region = NULL;
static size_t region_used = 0; /* bytes of the region carved into spans */
static size_t region_committed = 0;
static struct Page *slice_map[NUM_SLICES]; /* slice -> the page that owns it */

struct Pool {
    struct Page *head;
    size_t count;
    size_t keep;
};
static struct Pool pool_small = {NULL, 0, KEEP_SMALL};
static struct Pool pool_medium = {NULL, 0, KEEP_MEDIUM};
static struct Pool pool_grow = {NULL, 0, KEEP_GROW};

static atomic_flag big_lock = ATOMIC_FLAG_INIT;

static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&big_lock, memory_order_acquire)) {
        /* spin: held briefly, only when pages move to or from the pools */
    }
}

static void unlock(void) {
    atomic_flag_clear_explicit(&big_lock, memory_order_release);
}

/* ---------- OS calls ---------- */

static size_t os_page_size(void) {
    static size_t cached = 0;
    if (cached == 0) {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        cached = info.dwPageSize;
#else
        cached = (size_t)sysconf(_SC_PAGESIZE);
#endif
    }
    return cached;
}

static int region_init(void) {
#ifdef _WIN32
    region = VirtualAlloc(NULL, REGION_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
    return region != NULL;
#else
    void *p = mmap(NULL, REGION_RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return 0;
    region = p;
    return 1;
#endif
}

static int region_commit(size_t end) {
    if (end <= region_committed) return 1;
    size_t target = (end + COMMIT_STEP - 1) / COMMIT_STEP * COMMIT_STEP;
    if (target > REGION_RESERVE) target = REGION_RESERVE;
#ifdef _WIN32
    if (!VirtualAlloc(region + region_committed, target - region_committed, MEM_COMMIT, PAGE_READWRITE)) return 0;
#else
    if (mprotect(region + region_committed, target - region_committed, PROT_READ | PROT_WRITE) != 0) return 0;
#endif
    region_committed = target;
    return 1;
}

static void *os_map(size_t bytes) {
#ifdef _WIN32
    return VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void os_unmap(void *p, size_t bytes) {
#ifdef _WIN32
    (void)bytes;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, bytes);
#endif
}

/* Hand a span's memory back to the OS, except its first OS page, which
   holds the page header. Linux gives the pages back as zeros when touched. */
static void span_purge(struct Page *pg) {
    size_t keep = os_page_size();
    char *start = (char *)pg + keep;
    size_t len = pg->slices * SLICE - keep;
#ifdef _WIN32
    VirtualFree(start, len, MEM_DECOMMIT);
#else
    madvise(start, len, MADV_DONTNEED);
#endif
    pg->purged = 1;
}

static void span_unpurge(struct Page *pg) {
#ifdef _WIN32
    size_t keep = os_page_size();
    VirtualAlloc((char *)pg + keep, pg->slices * SLICE - keep, MEM_COMMIT, PAGE_READWRITE);
#endif
    pg->purged = 0;
}

/* ---------- size classes ---------- */

static unsigned log2_floor(size_t x) {
    return (unsigned)(sizeof(unsigned long long) * 8 - 1) - (unsigned)__builtin_clzll((unsigned long long)x);
}

/* Map a request size to its class. Assumes 0 < n <= MEDIUM_MAX. */
static inline unsigned class_of(size_t n) {
    if (n <= SMALL_MAX) return (unsigned)((n + 15) >> 4) - 1;
    unsigned b = log2_floor(n - 1);                                       /* 2^b < n <= 2^(b+1) */
    unsigned quarter = (unsigned)((n - 1 - ((size_t)1 << b)) >> (b - 2)); /* 0..3 */
    return SMALL_CLASSES + (b - 10) * 4 + quarter;
}

static size_t class_size(unsigned c) {
    if (c < SMALL_CLASSES) return ((size_t)c + 1) << 4;
    unsigned m = c - SMALL_CLASSES;
    unsigned b = 10 + m / 4;
    return ((size_t)1 << b) + ((size_t)(m % 4) + 1) * ((size_t)1 << (b - 2));
}

/* ---------- errors ---------- */

static void bad_free(void *ptr, const char *why) {
    fprintf(stderr, "burrow: bfree(%p): %s\n", ptr, why);
    abort();
}

/* ---------- spans: getting pages from the region or a pool ---------- */

static struct Pool *pool_for(uint32_t slices) {
    if (slices == SMALL_PAGE_SLICES) return &pool_small;
    if (slices == MEDIUM_PAGE_SLICES) return &pool_medium;
    return &pool_grow;
}

/* Get a span of `slices` slices: a pooled one if available, else fresh. */
static struct Page *span_get(uint32_t slices) {
    struct Pool *pool = pool_for(slices);
    struct Page *pg = NULL;
    int fresh = 0;
    lock();
    if (pool->head != NULL) {
        pg = pool->head;
        pool->head = pg->pool_next;
        pool->count--;
    } else {
        size_t bytes = (size_t)slices * SLICE;
        if ((region != NULL || region_init()) &&
            bytes <= REGION_RESERVE - region_used && region_commit(region_used + bytes)) {
            pg = (struct Page *)(region + region_used);
            size_t first = region_used >> SLICE_SHIFT;
            for (uint32_t i = 0; i < slices; i++) slice_map[first + i] = pg;
            region_used += bytes;
            fresh = 1;
        }
    }
    unlock();
    if (pg == NULL) return NULL;

    if (fresh) {
        pg->slices = slices;
        pg->purged = 0;
        pg->fresh = 1;
    } else if (pg->purged) {
        span_unpurge(pg);
        pg->fresh = 1; /* memory handed back and taken again comes back zeroed */
    } else {
        pg->fresh = 0;
    }
    return pg;
}

/* Return an empty span to its pool, so any size class can reuse it. */
static void span_put(struct Page *pg) {
    struct Pool *pool = pool_for(pg->slices);
    lock();
    int extra = pool->count >= pool->keep;
    pg->pool_next = pool->head;
    pool->head = pg;
    pool->count++;
    pg->heap = NULL;
    if (extra && !pg->purged) span_purge(pg);
    unlock();
}

/* ---------- giving each thread a heap ---------- */

#ifdef _WIN32
static DWORD exit_key = FLS_OUT_OF_INDEXES;
#else
static pthread_key_t exit_key;
#endif
static int exit_key_ready = 0;

#ifdef _WIN32
static void WINAPI on_thread_exit(void *heap)
#else
static void on_thread_exit(void *heap)
#endif
{
    if (heap == NULL || heap == FALLBACK) return;
    lock();
    ((struct ThreadHeap *)heap)->in_use = 0;
    unlock();
}

static struct ThreadHeap *acquire_heap(void) {
    struct ThreadHeap *h = NULL;
    lock();
    if (!exit_key_ready) {
#ifdef _WIN32
        exit_key = FlsAlloc(on_thread_exit);
        exit_key_ready = exit_key != FLS_OUT_OF_INDEXES ? 1 : -1;
#else
        exit_key_ready = pthread_key_create(&exit_key, on_thread_exit) == 0 ? 1 : -1;
#endif
        for (uint32_t i = 0; i <= MAX_HEAPS; i++) heaps[i].index = i;
        if (region == NULL) region_init();
    }
    /* Prefer a heap an exited thread left behind: it comes with its pages. */
    for (int i = 0; i < MAX_HEAPS; i++) {
        if (!heaps[i].in_use && heaps[i].ever_used) { h = &heaps[i]; break; }
    }
    if (h == NULL) {
        for (int i = 0; i < MAX_HEAPS; i++) {
            if (!heaps[i].in_use) { h = &heaps[i]; break; }
        }
    }
    if (h != NULL) {
        h->in_use = 1;
        h->ever_used = 1;
    }
    unlock();

    if (h == NULL) {
        h = FALLBACK; /* more than MAX_HEAPS live threads: shared, not safe */
    } else if (exit_key_ready == 1) {
#ifdef _WIN32
        FlsSetValue(exit_key, h);
#else
        pthread_setspecific(exit_key, h);
#endif
    }
    my_heap = h;
    return h;
}

static inline struct ThreadHeap *get_heap(void) {
    struct ThreadHeap *h = my_heap;
    return h ? h : acquire_heap();
}

/* ---------- finding a pointer's page ---------- */

static inline int in_region(const void *p) {
    return region != NULL && (uintptr_t)((const char *)p - region) < REGION_RESERVE;
}

static inline struct Page *page_of(const void *p) {
    return slice_map[(size_t)((const char *)p - region) >> SLICE_SHIFT];
}

/* ---------- a heap's page lists ---------- */

static void list_push(struct ThreadHeap *h, struct Page *pg) {
    struct Page **head = &h->pages[pg->cls];
    pg->prev = NULL;
    pg->next = *head;
    if (*head) (*head)->prev = pg;
    *head = pg;
    pg->in_list = 1;
}

static void list_remove(struct ThreadHeap *h, struct Page *pg) {
    if (pg->prev) pg->prev->next = pg->next;
    else h->pages[pg->cls] = pg->next;
    if (pg->next) pg->next->prev = pg->prev;
    pg->next = pg->prev = NULL;
    pg->in_list = 0;
}

/* Set up a span as a page of class c, owned by heap h. */
static void page_init(struct ThreadHeap *h, struct Page *pg, unsigned c) {
    pg->heap = h;
    pg->cls = (uint16_t)c;
    pg->kind = class_size(c) <= SMALL_PAGE_MAX ? KIND_SMALL : KIND_MEDIUM;
    pg->block_size = class_size(c);
    pg->start = (char *)pg + PAGE_HEADER;
    pg->bump = pg->start;
    size_t blocks = (pg->slices * SLICE - PAGE_HEADER) / pg->block_size;
    pg->limit = pg->start + blocks * pg->block_size;
    pg->free = NULL;
    pg->used = 0;
    list_push(h, pg);
}

/* ---------- freeing ---------- */

static void grow_free(struct ThreadHeap *h, struct Page *chunk, void *ptr);

static void local_free(struct ThreadHeap *h, struct Page *pg, void *ptr) {
    if (pg->kind == KIND_GROW) {
        grow_free(h, pg, ptr);
        return;
    }
    struct FreeNode *node = ptr;
    if (node == pg->free) bad_free(ptr, "double free");
#ifdef BURROW_DEBUG
    if ((size_t)((char *)ptr - pg->start) % pg->block_size != 0) bad_free(ptr, "not the start of a block");
    for (struct FreeNode *f = pg->free; f; f = f->next) {
        if (f == node) bad_free(ptr, "double free");
    }
#endif
    node->next = pg->free;
    pg->free = node;
    pg->used--;

    if (pg->used == 0 && (h->pages[pg->cls] != pg || pg->next != NULL)) {
        /* Completely empty, and not the only page of its class: give it
           back so any size class can use it. */
        if (pg->in_list) list_remove(h, pg);
        span_put(pg);
    } else if (!pg->in_list) {
        list_push(h, pg); /* was full, has room again */
    }
}

/* A block owned by another thread: send it to that thread's remote list.
   Lock-free: retry until no other thread changed the list under us. */
static void push_remote(struct ThreadHeap *owner, void *ptr) {
    struct FreeNode *node = ptr;
    struct FreeNode *head = atomic_load_explicit(&owner->remote, memory_order_relaxed);
    do {
        node->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&owner->remote, &head, node,
                                                    memory_order_release, memory_order_relaxed));
}

/* Free everything other threads sent back to us. */
static void drain_remote(struct ThreadHeap *h) {
    if (atomic_load_explicit(&h->remote, memory_order_relaxed) == NULL) return;
    struct FreeNode *node = atomic_exchange_explicit(&h->remote, NULL, memory_order_acquire);
    while (node != NULL) {
        struct FreeNode *next = node->next;
        local_free(h, page_of(node), node);
        node = next;
    }
}

/* ---------- allocating from pages ---------- */

/* Slow path: the current page had no free block. Sets *zeroed when the
   block is fresh memory that is already zero. */
static void *alloc_slow(struct ThreadHeap *h, unsigned c, int *zeroed) {
    drain_remote(h);
    struct Page *pg = h->pages[c];
    while (pg != NULL) {
        if (pg->free) {
            struct FreeNode *node = pg->free;
            pg->free = node->next;
            pg->used++;
            *zeroed = 0;
            return node;
        }
        if (pg->bump < pg->limit) {
            void *p = pg->bump;
            pg->bump += pg->block_size;
            pg->used++;
            *zeroed = pg->fresh;
            return p;
        }
        /* Full: take it off the list until one of its blocks is freed. */
        struct Page *next = pg->next;
        list_remove(h, pg);
        pg = next;
    }

    pg = span_get(class_size(c) <= SMALL_PAGE_MAX ? SMALL_PAGE_SLICES : MEDIUM_PAGE_SLICES);
    if (pg == NULL) return NULL;
    page_init(h, pg, c);
    void *p = pg->bump;
    pg->bump += pg->block_size;
    pg->used++;
    *zeroed = pg->fresh;
    return p;
}

static inline void *alloc_class(struct ThreadHeap *h, unsigned c, int *zeroed) {
    struct Page *pg = h->pages[c];
    if (pg != NULL) {
        struct FreeNode *node = pg->free;
        if (node != NULL) {
            /* Fastest: reuse a freed block. */
            pg->free = node->next;
            pg->used++;
            *zeroed = 0;
            return node;
        }
        if (pg->bump < pg->limit) {
            /* Next fastest: the next never-used block in this page. */
            void *p = pg->bump;
            pg->bump += pg->block_size;
            pg->used++;
            *zeroed = pg->fresh;
            return p;
        }
    }
    return alloc_slow(h, c, zeroed);
}

/* ---------- big blocks: their own OS mapping ---------- */

static void *big_alloc(size_t n) {
    size_t page = os_page_size();
    if (n > SIZE_MAX - SIZE_HEADER - page) return NULL;
    size_t bytes = (SIZE_HEADER + n + page - 1) / page * page;
    struct SizeHeader *hd = os_map(bytes);
    if (hd == NULL) return NULL;
    hd->size = bytes - SIZE_HEADER;
    hd->magic = BIG_MAGIC;
    return hd + 1;
}

/* ---------- grow chunks: where brealloc grows blocks in place ---------- */

static void *grow_alloc(struct ThreadHeap *h, size_t n) {
    size_t size = (n + 15) & ~(size_t)15;
    size_t need = SIZE_HEADER + size;
    struct Page *ch = h->grow;
    if (ch == NULL || (size_t)(ch->limit - ch->bump) < need) {
        if (ch != NULL && ch->used == 0) {
            ch->bump = ch->start; /* empty but too small for this: just reset */
        }
        if (ch == NULL || (size_t)(ch->limit - ch->bump) < need) {
            struct Page *fresh = span_get(GROW_CHUNK_SLICES);
            if (fresh == NULL) return NULL;
            if (ch != NULL && ch->used == 0) span_put(ch);
            ch = fresh;
            ch->heap = h;
            ch->kind = KIND_GROW;
            ch->start = (char *)ch + PAGE_HEADER;
            ch->bump = ch->start;
            ch->limit = (char *)ch + ch->slices * SLICE;
            ch->used = 0;
            ch->in_list = 0;
            h->grow = ch;
        }
    }
    struct SizeHeader *hd = (struct SizeHeader *)ch->bump;
    ch->bump += need;
    ch->used++;
    hd->size = size;
    hd->magic = GROW_MAGIC;
    return hd + 1;
}

static void grow_free(struct ThreadHeap *h, struct Page *ch, void *ptr) {
    struct SizeHeader *hd = (struct SizeHeader *)ptr - 1;
    if (hd->magic != GROW_MAGIC) bad_free(ptr, hd->magic == 0 ? "double free" : "not a block start");
    hd->magic = 0;
    if ((char *)ptr + hd->size == ch->bump) ch->bump = (char *)hd; /* last one: roll back */
    ch->used--;
    if (ch->used == 0) {
        if (ch == h->grow) ch->bump = ch->start; /* empty: start over */
        else span_put(ch);                       /* an old chunk: give it back */
    }
}

/* ---------- public API ---------- */

void *balloc(size_t n) {
    if (n == 0) return NULL;
    if (n > MEDIUM_MAX) return big_alloc(n);
    int zeroed;
    return alloc_class(get_heap(), class_of(n), &zeroed);
}

void bfree(void *ptr) {
    if (ptr == NULL) return;
    if (in_region(ptr)) {
        struct Page *pg = page_of(ptr);
        if (pg == NULL || pg->heap == NULL) bad_free(ptr, "pointer is not in a live page");
        struct ThreadHeap *h = get_heap();
        if (pg->heap != h) {
            push_remote(pg->heap, ptr); /* send it home */
            return;
        }
        local_free(h, pg, ptr);
        return;
    }
    struct SizeHeader *hd = (struct SizeHeader *)ptr - 1;
    if (hd->magic != BIG_MAGIC) bad_free(ptr, "pointer was not allocated by balloc");
    hd->magic = 0;
    os_unmap(hd, hd->size + SIZE_HEADER);
}

void *bcalloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) return NULL; /* count * size would overflow */
    size_t total = count * size;
    if (total == 0) return NULL;
    if (total > MEDIUM_MAX) return big_alloc(total); /* new OS mappings are always zero */
    int zeroed;
    void *p = alloc_class(get_heap(), class_of(total), &zeroed);
    if (p != NULL && !zeroed) memset(p, 0, total);
    return p;
}

size_t burrow_usable(void *ptr) {
    if (ptr == NULL) return 0;
    if (in_region(ptr)) {
        struct Page *pg = page_of(ptr);
        if (pg->kind == KIND_GROW) return ((struct SizeHeader *)ptr - 1)->size;
        return pg->block_size;
    }
    return ((struct SizeHeader *)ptr - 1)->size;
}

void *brealloc(void *ptr, size_t n) {
    if (ptr == NULL) return balloc(n);
    if (n == 0) {
        bfree(ptr);
        return NULL;
    }
    size_t have = burrow_usable(ptr);
    if (n <= have) return ptr; /* still fits */

    struct ThreadHeap *h = get_heap();
    if (n <= GROW_MAX) {
        /* Already the last block in our grow chunk: just grow it. */
        if (in_region(ptr)) {
            struct Page *pg = page_of(ptr);
            if (pg->kind == KIND_GROW && pg->heap == h && (char *)ptr + have == pg->bump) {
                size_t extra = ((n + 15) & ~(size_t)15) - have;
                if ((size_t)(pg->limit - pg->bump) >= extra) {
                    pg->bump += extra;
                    ((struct SizeHeader *)ptr - 1)->size += extra;
                    return ptr;
                }
            }
        }
        /* Otherwise move it to the end of the grow chunk, where it can keep
           growing in place next time. */
        void *fresh = grow_alloc(h, n);
        if (fresh != NULL) {
            memcpy(fresh, ptr, have);
            bfree(ptr);
            return fresh;
        }
    }
    void *fresh = balloc(n);
    if (fresh == NULL) return NULL; /* old block stays valid, like realloc */
    memcpy(fresh, ptr, have);
    bfree(ptr);
    return fresh;
}

size_t burrow_committed(void) {
    lock();
    size_t c = region_committed;
    unlock();
    return c;
}
