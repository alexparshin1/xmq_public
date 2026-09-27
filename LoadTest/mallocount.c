/*
 * Counts allocations for one run of a scenario, so two versions of the broker can be compared by
 * the work they ask of the allocator rather than by a latency this bench cannot resolve.
 *
 * Interposes the glibc entry points and forwards to __libc_* - not to a dlsym() lookup, which
 * itself allocates and deadlocks during startup.
 *
 * Build: gcc -O2 -shared -fPIC -o mallocount.so mallocount.c
 * Use:   MALLOCOUNT_OUT=/path/to/file LD_PRELOAD=/path/to/mallocount.so <program>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

extern void*  __libc_malloc(size_t);
extern void*  __libc_calloc(size_t, size_t);
extern void*  __libc_realloc(void*, size_t);
extern void   __libc_free(void*);

static _Atomic unsigned long long counts[4];   /* malloc, calloc, realloc, free */
static _Atomic unsigned long long bytes;

enum { MALLOC = 0, CALLOC = 1, REALLOC = 2, FREE = 3 };

void* malloc(size_t size)
{
    __atomic_fetch_add(&counts[MALLOC], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&bytes, size, __ATOMIC_RELAXED);
    return __libc_malloc(size);
}

void* calloc(size_t count, size_t size)
{
    __atomic_fetch_add(&counts[CALLOC], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&bytes, count * size, __ATOMIC_RELAXED);
    return __libc_calloc(count, size);
}

void* realloc(void* pointer, size_t size)
{
    __atomic_fetch_add(&counts[REALLOC], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&bytes, size, __ATOMIC_RELAXED);
    return __libc_realloc(pointer, size);
}

void free(void* pointer)
{
    if (pointer)
    {
        __atomic_fetch_add(&counts[FREE], 1, __ATOMIC_RELAXED);
    }
    __libc_free(pointer);
}

/* Written from a signal handler as well as at exit, so it uses write(2) and no allocation. */
static void report(void)
{
    const char* path = getenv("MALLOCOUNT_OUT");
    char        line[256];
    const int   length = snprintf(line, sizeof(line),
                                  "malloc=%llu calloc=%llu realloc=%llu free=%llu bytes=%llu\n",
                                  __atomic_load_n(&counts[MALLOC], __ATOMIC_RELAXED),
                                  __atomic_load_n(&counts[CALLOC], __ATOMIC_RELAXED),
                                  __atomic_load_n(&counts[REALLOC], __ATOMIC_RELAXED),
                                  __atomic_load_n(&counts[FREE], __ATOMIC_RELAXED),
                                  __atomic_load_n(&bytes, __ATOMIC_RELAXED));
    int fd = 2;
    if (path)
    {
        const int opened = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (opened >= 0) fd = opened;
    }
    (void) !write(fd, line, (size_t) length);
    if (fd != 2) close(fd);
}

__attribute__((destructor)) static void onExit(void) { report(); }
