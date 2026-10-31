/* A tiny cross-platform thread wrapper for the tests and benchmark. */
#ifndef BURROW_THREADS_H
#define BURROW_THREADS_H

#ifdef _WIN32
#include <windows.h>
typedef HANDLE thread_t;
typedef DWORD (WINAPI *thread_entry_t)(LPVOID);
#define THREAD_FN(name) DWORD WINAPI name(LPVOID arg)
#define THREAD_RETURN return 0
static int thread_start(thread_t *t, thread_entry_t fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t != NULL;
}
static void thread_join(thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
#include <pthread.h>
typedef pthread_t thread_t;
#define THREAD_FN(name) void *name(void *arg)
#define THREAD_RETURN return NULL
static int thread_start(thread_t *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0;
}
static void thread_join(thread_t t) {
    pthread_join(t, NULL);
}
#endif

#endif
