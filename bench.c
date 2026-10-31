/* Speed race: Burrow vs the system malloc (and mimalloc, if built with -DUSE_MIMALLOC). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 199309L
#endif

#include "burrow.h"
#include "threads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_MIMALLOC
#include <mimalloc.h>
#endif

#ifdef _WIN32
#include <psapi.h>
static double now_sec(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
static double now_sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
#endif

/* Every allocator is called through the same function pointers, so none
   gets an unfair advantage from how it's called. */
typedef struct {
    const char *name;
    void *(*alloc)(size_t);
    void *(*zalloc)(size_t, size_t);
    void *(*re)(void *, size_t);
    void (*release)(void *);
} Allocator;

static const Allocator ALLOCATORS[] = {
    {"burrow", balloc, bcalloc, brealloc, bfree},
    {"malloc", malloc, calloc, realloc, free},
#ifdef USE_MIMALLOC
    {"mimalloc", mi_malloc, mi_calloc, mi_realloc, mi_free},
#endif
};
#define NUM_ALLOCATORS ((int)(sizeof(ALLOCATORS) / sizeof(ALLOCATORS[0])))

enum { COUNT = 10000, ROUNDS = 5, RANDOM_OPS = 200000, LIVE = 2000, RUNS = 15 };

static unsigned next_rand(unsigned *s) {
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7fff;
}

/* ---------- single-thread patterns ---------- */

static void *slots[COUNT];

static double same_size(const Allocator *a) {
    double t = now_sec();
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < COUNT; i++) slots[i] = a->alloc(32);
        for (int i = 0; i < COUNT; i++) a->release(slots[i]);
    }
    return (now_sec() - t) / (2.0 * COUNT * ROUNDS);
}

static double lifo(const Allocator *a) {
    double t = now_sec();
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < COUNT; i++) slots[i] = a->alloc(64);
        for (int i = COUNT - 1; i >= 0; i--) a->release(slots[i]);
    }
    return (now_sec() - t) / (2.0 * COUNT * ROUNDS);
}

static double random_mix_run(const Allocator *a, unsigned seed, void **mine) {
    for (int i = 0; i < LIVE; i++) mine[i] = NULL;
    double t = now_sec();
    for (int op = 0; op < RANDOM_OPS; op++) {
        int i = (int)(next_rand(&seed) % LIVE);
        if (mine[i]) {
            a->release(mine[i]);
            mine[i] = NULL;
        } else {
            mine[i] = a->alloc(16 + next_rand(&seed) % 497);
        }
    }
    double elapsed = now_sec() - t;
    for (int i = 0; i < LIVE; i++) if (mine[i]) a->release(mine[i]);
    return elapsed;
}

static double random_mix(const Allocator *a) {
    return random_mix_run(a, 42, slots) / RANDOM_OPS;
}

static double zeroed(const Allocator *a) {
    double t = now_sec();
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < COUNT; i++) slots[i] = a->zalloc(16, 4);
        for (int i = 0; i < COUNT; i++) a->release(slots[i]);
    }
    return (now_sec() - t) / (2.0 * COUNT * ROUNDS);
}

static double growing(const Allocator *a) {
    long ops = 0;
    double t = now_sec();
    for (int i = 0; i < 10000; i++) {
        size_t cap = 8;
        void *p = a->re(NULL, cap);
        ops++;
        while (cap < 64 * 1024) {
            cap *= 2;
            p = a->re(p, cap);
            ops++;
        }
        a->release(p);
        ops++;
    }
    return (now_sec() - t) / (double)ops;
}

/* ---------- multithread pattern ----------
   Every thread runs the random mix at the same time. Time per operation is
   wall time divided by all operations, so better scaling = smaller number. */

enum { MAX_THREADS = 8 };
static const Allocator *mt_alloc;
static void *mt_slots[MAX_THREADS][LIVE];

static THREAD_FN(mt_worker) {
    int id = (int)(size_t)arg;
    random_mix_run(mt_alloc, 1000u + (unsigned)id, mt_slots[id]);
    THREAD_RETURN;
}

static double threaded(const Allocator *a, int n) {
    thread_t t[MAX_THREADS];
    mt_alloc = a;
    double start = now_sec();
    for (int i = 0; i < n; i++) thread_start(&t[i], mt_worker, (void *)(size_t)i);
    for (int i = 0; i < n; i++) thread_join(t[i]);
    return (now_sec() - start) / ((double)RANDOM_OPS * n);
}
static double threads4(const Allocator *a) { return threaded(a, 4); }
static double threads8(const Allocator *a) { return threaded(a, 8); }

/* ---------- table ---------- */

typedef struct {
    const char *name;
    double (*run)(const Allocator *);
} Pattern;

static const Pattern PATTERNS[] = {
    {"same size, free in order", same_size},
    {"same size, free reversed", lifo},
    {"random sizes and order", random_mix},
    {"zeroed (calloc)", zeroed},
    {"growing arrays (realloc)", growing},
    {"random mix, 4 threads", threads4},
    {"random mix, 8 threads", threads8},
};

/* ---------- memory use ----------
   Each allocator runs each memory pattern in a separate copy of this
   program, so they can't share or hide memory. The child reports how much
   real memory the process used at its peak, minus what it used at start. */

static size_t mem_current(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.WorkingSetSize;
#else
    size_t pages = 0, resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%zu %zu", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    return resident * (size_t)sysconf(_SC_PAGESIZE);
#endif
}

static size_t mem_peak(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.PeakWorkingSetSize;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (size_t)ru.ru_maxrss * 1024; /* Linux reports KB */
#endif
}

/* Every memory pattern returns the most bytes the program actually asked
   for at once: the ideal an allocator with zero overhead would use. */
static size_t live_now, live_peak;
static void *track_alloc(const Allocator *a, size_t n) {
    void *p = a->alloc(n);
    live_now += n;
    if (live_now > live_peak) live_peak = live_now;
    return p;
}
static void track_free(const Allocator *a, void *p, size_t n) {
    a->release(p);
    live_now -= n;
}

enum { MEM_SLOTS = 300000 };
static void *mslot[MEM_SLOTS];
static size_t msize[MEM_SLOTS];

/* Lots of small objects, all alive at once. Shows per-block overhead. */
static void mem_small(const Allocator *a) {
    unsigned seed = 7;
    for (int i = 0; i < MEM_SLOTS; i++) {
        msize[i] = 16 + next_rand(&seed) % 113;
        mslot[i] = track_alloc(a, msize[i]);
        memset(mslot[i], 1, msize[i]);
    }
    for (int i = 0; i < MEM_SLOTS; i++) track_free(a, mslot[i], msize[i]);
}

/* Medium sizes from 1 KB to 64 KB. Shows rounding waste. */
static void mem_medium(const Allocator *a) {
    unsigned seed = 11;
    int n = 3000;
    for (int i = 0; i < n; i++) {
        msize[i] = 1024 + (next_rand(&seed) * 7u) % (63 * 1024);
        mslot[i] = track_alloc(a, msize[i]);
        memset(mslot[i], 1, msize[i]);
    }
    for (int i = 0; i < n; i++) track_free(a, mslot[i], msize[i]);
}

/* The program's sizes shift halfway through: small objects first, then
   bigger ones. Memory freed as small blocks can't serve the big ones unless
   the allocator can reuse across sizes. */
static void mem_shift(const Allocator *a) {
    unsigned seed = 13;
    int n = 20000;
    for (int i = 0; i < n; i++) mslot[i] = NULL;
    for (int phase = 0; phase < 2; phase++) {
        for (int op = 0; op < 400000; op++) {
            int i = (int)(next_rand(&seed) % n);
            if (mslot[i]) {
                track_free(a, mslot[i], msize[i]);
                mslot[i] = NULL;
            } else {
                msize[i] = phase == 0 ? 256 + next_rand(&seed) % 769 : 2048 + next_rand(&seed) % 2049;
                mslot[i] = track_alloc(a, msize[i]);
                memset(mslot[i], 1, msize[i]);
            }
        }
        /* end of a phase: free everything from it */
        for (int i = 0; i < n; i++) if (mslot[i]) { track_free(a, mslot[i], msize[i]); mslot[i] = NULL; }
    }
}

/* One thread makes blocks, another frees them, over and over. Shows whether
   memory flows back to the thread that needs it or piles up elsewhere. */
enum { HAND_N = 20000, HAND_ROUNDS = 50 };
static const Allocator *hand_alloc;
static void *hand[HAND_N];
static THREAD_FN(hand_free_all) {
    (void)arg;
    for (int i = 0; i < HAND_N; i++) hand_alloc->release(hand[i]);
    THREAD_RETURN;
}
static void mem_handoff(const Allocator *a) {
    hand_alloc = a;
    live_peak = (size_t)HAND_N * 64;
    for (int r = 0; r < HAND_ROUNDS; r++) {
        for (int i = 0; i < HAND_N; i++) {
            hand[i] = a->alloc(64);
            memset(hand[i], 1, 64);
        }
        thread_t t;
        thread_start(&t, hand_free_all, NULL);
        thread_join(t);
    }
}

typedef struct {
    const char *name;
    void (*run)(const Allocator *);
} MemPattern;

static const MemPattern MEM_PATTERNS[] = {
    {"300k small objects", mem_small},
    {"3k medium (1-64 KB)", mem_medium},
    {"sizes shift midway", mem_shift},
    {"made/freed on 2 threads", mem_handoff},
};
#define NUM_MEM_PATTERNS ((int)(sizeof(MEM_PATTERNS) / sizeof(MEM_PATTERNS[0])))

/* Child process: run one pattern with one allocator and report. */
static int mem_child(int a, int p) {
    size_t start = mem_current();
    live_now = live_peak = 0;
    MEM_PATTERNS[p].run(&ALLOCATORS[a]);
    size_t peak = mem_peak();
    printf("%zu %zu\n", peak > start ? peak - start : 0, live_peak);
    return 0;
}

static int mem_report(const char *self) {
    char exe[1024];
    snprintf(exe, sizeof exe, "%s", self);
#ifdef _WIN32
    for (char *c = exe; *c; c++) if (*c == '/') *c = '\\'; /* cmd.exe wants backslashes */
#endif
    printf("peak memory used, in MB (lower is better); \"needed\" is what the program\n");
    printf("actually asked for at its busiest, so the gap above it is overhead\n\n");
    printf("%-26s%10s", "pattern", "needed");
    for (int a = 0; a < NUM_ALLOCATORS; a++) printf("%18s", ALLOCATORS[a].name);
    printf("\n");
    for (int p = 0; p < NUM_MEM_PATTERNS; p++) {
        double needed = 0;
        double used[8] = {0};
        for (int a = 0; a < NUM_ALLOCATORS; a++) {
            char cmd[1200];
            snprintf(cmd, sizeof cmd, "\"%s\" memrun %d %d", exe, a, p);
            FILE *f = popen(cmd, "r");
            size_t peak = 0, live = 0;
            if (f) {
                if (fscanf(f, "%zu %zu", &peak, &live) != 2) peak = live = 0;
                pclose(f);
            }
            used[a] = (double)peak / (1024.0 * 1024.0);
            needed = (double)live / (1024.0 * 1024.0);
        }
        printf("%-26s%8.1f MB", MEM_PATTERNS[p].name, needed);
        for (int a = 0; a < NUM_ALLOCATORS; a++) {
            printf("%9.1f MB (%3.0f%%)", used[a], needed > 0 ? 100.0 * used[a] / needed : 0.0);
        }
        printf("\n");
    }
    printf("\n(%% = peak memory as a percentage of what was needed; 100%% is perfect)\n");
    return 0;
}

/* ---------- statistics ---------- */

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Sorts the samples, then reads off the middle value and the quarter
   points. Half the runs land between p25 and p75: that's the spread. */
typedef struct {
    double median, p25, p75;
} Stats;

static Stats summarize(double *samples, int n) {
    qsort(samples, (size_t)n, sizeof(double), cmp_double);
    Stats st;
    st.median = samples[n / 2];
    st.p25 = samples[n / 4];
    st.p75 = samples[(3 * n) / 4];
    return st;
}

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "memrun") == 0) return mem_child(atoi(argv[2]), atoi(argv[3]));
    if (argc >= 2 && strcmp(argv[1], "mem") == 0) return mem_report(argv[0]);

    printf("time per operation: median of %d runs, +/- half the middle spread (lower is better)\n", RUNS);
    printf("\"tie\" means the leaders' spreads overlap, so the difference is within noise\n\n");
    printf("%-26s", "pattern");
    for (int a = 0; a < NUM_ALLOCATORS; a++) printf("%18s", ALLOCATORS[a].name);
    printf("   verdict\n");

    for (size_t p = 0; p < sizeof(PATTERNS) / sizeof(PATTERNS[0]); p++) {
        double samples[8][RUNS];

        /* Take turns: each round runs every allocator once, starting with a
           different one each time, so a background hiccup can't land on just
           one allocator's runs. */
        for (int r = 0; r < RUNS; r++) {
            for (int k = 0; k < NUM_ALLOCATORS; k++) {
                int a = (r + k) % NUM_ALLOCATORS;
                samples[a][r] = PATTERNS[p].run(&ALLOCATORS[a]);
            }
        }

        Stats st[8];
        int first = 0, second = -1;
        printf("%-26s", PATTERNS[p].name);
        for (int a = 0; a < NUM_ALLOCATORS; a++) {
            st[a] = summarize(samples[a], RUNS);
            double half_spread = (st[a].p75 - st[a].p25) / 2.0;
            printf("%9.1f +/-%4.1f ns", st[a].median * 1e9, half_spread * 1e9);
        }
        for (int a = 1; a < NUM_ALLOCATORS; a++) {
            if (st[a].median < st[first].median) first = a;
        }
        for (int a = 0; a < NUM_ALLOCATORS; a++) {
            if (a == first) continue;
            if (second < 0 || st[a].median < st[second].median) second = a;
        }

        /* Overlapping spreads: the leader's slow runs reach the runner-up's fast runs. */
        if (second >= 0 && st[first].p75 >= st[second].p25) {
            printf("   tie: %s / %s\n", ALLOCATORS[first].name, ALLOCATORS[second].name);
        } else {
            double ratio = second >= 0 ? st[second].median / st[first].median : 1.0;
            printf("   %s (%.2fx)\n", ALLOCATORS[first].name, ratio);
        }
    }
    return 0;
}
