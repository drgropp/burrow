#include "burrow.h"
#include "threads.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

static int failures = 0;

static void check(int ok, const char *what) {
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int aligned(void *p) {
    return ((size_t)p % alignof(max_align_t)) == 0;
}

/* ---- multithreaded tests ---- */

enum { THREADS = 8, THREAD_ITERS = 20000, THREAD_SLOTS = 256 };
static int thread_ok[THREADS];

/* Each thread allocates, fills with its own pattern, checks, and frees,
   all at the same time as the others. Any mix-up shows as a wrong byte. */
static THREAD_FN(worker) {
    int id = (int)(size_t)arg;
    unsigned seed = 1234u + (unsigned)id;
    unsigned char *slots[THREAD_SLOTS] = {0};
    size_t sizes[THREAD_SLOTS] = {0};
    int ok = 1;
    for (int i = 0; i < THREAD_ITERS; i++) {
        seed = seed * 1103515245u + 12345u;
        int k = (int)((seed >> 16) % THREAD_SLOTS);
        if (slots[k]) {
            unsigned char want = (unsigned char)(id * 31 + k);
            for (size_t j = 0; j < sizes[k]; j++) if (slots[k][j] != want) ok = 0;
            bfree(slots[k]);
            slots[k] = NULL;
        } else {
            sizes[k] = 1 + (seed >> 8) % 2000;
            slots[k] = balloc(sizes[k]);
            if (!slots[k]) { ok = 0; continue; }
            memset(slots[k], id * 31 + k, sizes[k]);
        }
    }
    for (int k = 0; k < THREAD_SLOTS; k++) bfree(slots[k]);
    thread_ok[id] = ok;
    THREAD_RETURN;
}

/* One thread makes blocks, another checks and frees them. */
enum { HANDOFF = 10000 };
static unsigned char *handoff[HANDOFF];
static int consumer_ok;

static THREAD_FN(producer) {
    (void)arg;
    for (int i = 0; i < HANDOFF; i++) {
        handoff[i] = balloc(64);
        if (handoff[i]) memset(handoff[i], i & 0xff, 64);
    }
    THREAD_RETURN;
}

static THREAD_FN(consumer) {
    (void)arg;
    int ok = 1;
    for (int i = 0; i < HANDOFF; i++) {
        if (!handoff[i]) { ok = 0; continue; }
        for (int j = 0; j < 64; j++) if (handoff[i][j] != (i & 0xff)) ok = 0;
        bfree(handoff[i]);
    }
    /* reuse what was just freed, from this thread */
    for (int i = 0; i < HANDOFF; i++) {
        unsigned char *p = balloc(64);
        if (!p) { ok = 0; break; }
        memset(p, 0x5a, 64);
        bfree(p);
    }
    consumer_ok = ok;
    THREAD_RETURN;
}

/* A short-lived thread that allocates a bit and exits. */
static THREAD_FN(short_lived) {
    (void)arg;
    void *p[100];
    for (int i = 0; i < 100; i++) p[i] = balloc(1000);
    for (int i = 0; i < 100; i += 2) bfree(p[i]); /* leave some live: they're "leaked" on purpose */
    THREAD_RETURN;
}

/* Blocks made on one thread and freed on another must go back to the
   thread that made them, so it can reuse them. */
enum { RETURN_N = 2000 };
static void *made[RETURN_N];
static atomic_int phase = 0;
static int returned_ok = 0;

static THREAD_FN(maker) {
    (void)arg;
    for (int i = 0; i < RETURN_N; i++) made[i] = balloc(48);
    atomic_store(&phase, 1);
    while (atomic_load(&phase) != 2) { /* wait for main thread to free them */ }
    /* The second round should come from the same pages the first round used. */
    char *lo = made[0], *hi = made[0];
    for (int i = 0; i < RETURN_N; i++) {
        if ((char *)made[i] < lo) lo = made[i];
        if ((char *)made[i] > hi) hi = made[i];
    }
    int reused = 0;
    for (int i = 0; i < RETURN_N; i++) {
        char *p = balloc(48);
        if (p >= lo - 4096 && p <= hi + 65536) reused++;
    }
    returned_ok = reused == RETURN_N;
    THREAD_RETURN;
}

int main(void) {
    char *a = balloc(64);
    char *b = balloc(64);
    check(a != NULL && b != NULL, "basic allocations succeed");
    check(balloc(0) == NULL, "balloc(0) returns NULL");
    check(balloc(SIZE_MAX) == NULL, "impossible size returns NULL");
    check(burrow_usable(a) >= 64, "block holds at least what was asked for");

    bfree(a);
    char *c = balloc(60);
    check(c == a, "freed block reused by a request of the same size class");
    char *d = balloc(500);
    check(d != a && d != b, "a different size class gets its own block");

    char *e = balloc(3);
    char *f = balloc(1000);
    char *g = balloc(5000);
    check(aligned(b) && aligned(c) && aligned(e) && aligned(f) && aligned(g), "every size gives an aligned pointer");

    bfree(NULL);
    check(1, "bfree(NULL) does nothing");

    /* big blocks get their own OS mapping */
    size_t big = (size_t)10 << 20;
    unsigned char *m = balloc(big);
    int ok = m != NULL;
    if (ok) {
        for (size_t i = 0; i < big; i++) m[i] = (unsigned char)i;
        for (size_t i = 0; i < big; i++) if (m[i] != (unsigned char)i) ok = 0;
    }
    check(ok, "10 MB block from its own OS mapping, every byte writable");
    bfree(m);

    /* bcalloc: fresh and reused blocks both come back zeroed */
    unsigned char *z = bcalloc(1000, 4);
    ok = z != NULL;
    for (int i = 0; ok && i < 4000; i++) if (z[i]) ok = 0;
    check(ok, "bcalloc: fresh block is zero");
    memset(z, 0xAB, 4000);
    bfree(z);
    z = bcalloc(1000, 4);
    ok = z != NULL;
    for (int i = 0; ok && i < 4000; i++) if (z[i]) ok = 0;
    check(ok, "bcalloc: reused (dirty) block is zeroed again");
    check(bcalloc(SIZE_MAX / 2, 4) == NULL, "bcalloc catches count * size overflow");
    bfree(z);

    /* brealloc */
    char *r = brealloc(NULL, 10);
    check(r != NULL, "brealloc(NULL, n) acts like balloc");
    strcpy(r, "burrow!");
    char *r2 = brealloc(r, 8);
    check(r2 == r, "brealloc to a size that still fits returns the same pointer");
    r2 = brealloc(r, 5000);
    check(r2 != NULL && strcmp(r2, "burrow!") == 0, "brealloc to a bigger size keeps the contents");
    check(brealloc(r2, 0) == NULL, "brealloc(p, 0) frees and returns NULL");

    /* stress: patterns, half freed and refilled, verify nothing overlaps */
    enum { N = 5000 };
    static char *ptrs[N];
    static size_t sizes[N];
    for (int k = 0; k < N; k++) {
        sizes[k] = (size_t)(k * 37 % 3000) + 1;
        ptrs[k] = balloc(sizes[k]);
        if (ptrs[k]) memset(ptrs[k], k & 0xff, sizes[k]);
    }
    for (int k = 0; k < N; k += 2) { bfree(ptrs[k]); ptrs[k] = NULL; }
    for (int k = 0; k < N; k += 2) {
        ptrs[k] = balloc(sizes[k]);
        if (ptrs[k]) memset(ptrs[k], k & 0xff, sizes[k]);
    }
    ok = 1;
    for (int k = 0; k < N; k++) {
        if (!ptrs[k] || !aligned(ptrs[k])) { ok = 0; continue; }
        for (size_t j = 0; j < sizes[k]; j++) if ((unsigned char)ptrs[k][j] != (k & 0xff)) ok = 0;
    }
    check(ok, "5000 mixed allocations: no overlaps, all aligned");
    for (int k = 0; k < N; k++) bfree(ptrs[k]);

    thread_t t[THREADS];
    int started = 1;
    for (int i = 0; i < THREADS; i++) started &= thread_start(&t[i], worker, (void *)(size_t)i);
    for (int i = 0; i < THREADS; i++) thread_join(t[i]);
    ok = started;
    for (int i = 0; i < THREADS; i++) ok &= thread_ok[i];
    check(ok, "8 threads allocating and freeing at once: no corruption");

    thread_t prod, cons;
    thread_start(&prod, producer, NULL);
    thread_join(prod);
    thread_start(&cons, consumer, NULL);
    thread_join(cons);
    check(consumer_ok, "blocks made on one thread, checked and freed on another");

    thread_t mk;
    thread_start(&mk, maker, NULL);
    while (atomic_load(&phase) != 1) { }
    for (int i = 0; i < RETURN_N; i++) bfree(made[i]);
    atomic_store(&phase, 2);
    thread_join(mk);
    check(returned_ok, "blocks freed by another thread return to their owner and get reused");

    check(burrow_usable(balloc(1100)) == 1280, "1100 bytes rounds to 1280, not 2048 (finer medium classes)");

    /* empty pages go back to a shared pool and get reused by any size class */
    {
        enum { M = 20000 };
        static void *mb[M];
        for (int i = 0; i < M; i++) mb[i] = balloc(64);     /* fills about 20 pages */
        for (int i = 0; i < M; i++) bfree(mb[i]);
        size_t c1 = burrow_committed();
        for (int i = 0; i < M / 4; i++) mb[i] = balloc(200);    /* a different size class, similar total */
        for (int i = 0; i < M / 4; i++) bfree(mb[i]);
        size_t c2 = burrow_committed();
        check(c2 == c1, "empty pages are reused by a different size class (no new memory needed)");
    }

    /* lots of medium memory freed: extra pages hand memory back, and still work when reused */
    {
        enum { M = 200 };
        static unsigned char *mb[M];
        for (int i = 0; i < M; i++) { mb[i] = balloc(100 * 1024); memset(mb[i], 0x5a, 100 * 1024); }
        for (int i = 0; i < M; i++) bfree(mb[i]);
        ok = 1;
        for (int i = 0; i < M; i++) {
            mb[i] = balloc(100 * 1024);
            if (!mb[i]) { ok = 0; continue; }
            memset(mb[i], 0x33, 100 * 1024);
            if (mb[i][100 * 1024 - 1] != 0x33) ok = 0;
        }
        for (int i = 0; i < M; i++) bfree(mb[i]);
        check(ok, "medium pages reused after their memory was handed back to the OS");
    }

    /* bcalloc on memory from a recycled page is still zero */
    {
        unsigned char *d1 = balloc(500);
        memset(d1, 0xEE, 500);
        bfree(d1);
        unsigned char *z2 = bcalloc(1, 500);
        ok = z2 != NULL;
        for (int i = 0; ok && i < 500; i++) if (z2[i]) ok = 0;
        check(ok, "bcalloc on a recycled block is zero");
        bfree(z2);
    }

    /* 200 threads one after another: each should reuse the heap the last
       one left behind instead of taking a fresh 4 MB chunk every time. */
    size_t before = burrow_committed();
    for (int i = 0; i < 200; i++) {
        thread_t st;
        thread_start(&st, short_lived, NULL);
        thread_join(st);
    }
    size_t grew = burrow_committed() - before;
    check(grew < ((size_t)40 << 20), "200 short-lived threads recycle heaps instead of piling up memory");

    printf("\n%s (%d failed)\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
