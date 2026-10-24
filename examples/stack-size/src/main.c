/* What a thread is given when it asks for a stack, measured.
 *
 * mcpplibs/openkal-musl#48: pthread_attr_setstacksize was accepted and every
 * thread ran on 256 KiB whatever it asked for; the size asked for was mapped
 * for a thread that never ran on it and committed in full; a stack the caller
 * supplied was not used and pthread_create reported success. Each line below
 * is one of those, observed rather than read back:
 *
 *   room        a thread that asked for 4 MiB recurses through 3 MiB, and is
 *               told at least 4 MiB by pthread_getattr_np
 *   default     a thread that asked for nothing recurses through 6 MiB, the
 *               room the host's own C library gives one
 *   resident    asking for 256 MiB does not raise the resident size by it
 *   supplied    a stack the caller supplies is refused, not ignored
 *   guard       a read just below a thread's stack ends the program that made
 *               it, rather than reading whatever mapping lies there
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static int failures = 0;

static void check(int ok, const char* what) {
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
    if (!ok) failures++;
}

static long recurse(long n);
static long (*volatile g_recurse)(long) = recurse;
static long recurse(long n) {
    volatile char pad[1000];
    pad[0] = (char)n;
    if (n == 0) return 0;
    return g_recurse(n - 1) + pad[0];
}

struct probe { long kib; size_t told; int done; };

static void* body(void* p) {
    struct probe* q = p;
    pthread_attr_t a;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        void* base; size_t size;
        if (pthread_attr_getstack(&a, &base, &size) == 0) q->told = size;
        pthread_attr_destroy(&a);
    }
    g_recurse(q->kib);
    q->done = 1;
    return 0;
}

static int run(size_t ask, long kib, struct probe* q) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (ask) pthread_attr_setstacksize(&attr, ask);
    pthread_t t;
    q->kib = kib; q->told = 0; q->done = 0;
    const int rc = pthread_create(&t, &attr, body, q);
    pthread_attr_destroy(&attr);
    if (rc == 0) pthread_join(t, 0);
    return rc;
}

static long resident_kib(void) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256]; long kib = -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "VmRSS:", 6) == 0) { kib = strtol(line + 6, 0, 10); break; }
    fclose(f);
    return kib;
}

/* Run in a program of its own, which this one starts: the read is meant to end
 * the program it is made in. What it prints says what it was about to read,
 * so a read that does not fault is a fact with an address rather than a bare
 * failure. */
static void* touch_below(void* unused) {
    (void)unused;
    pthread_attr_t a; void* base = 0; size_t size = 0;
    const int e = pthread_getattr_np(pthread_self(), &a);
    if (e == 0) {
        pthread_attr_getstack(&a, &base, &size);
        pthread_attr_destroy(&a);
    }
    char here = 0;
    printf("touch: getattr=%d base=%p size=%zu local=%p reading %p\n",
           e, base, size, (void*)&here, base ? (void*)((char*)base - 1) : (void*)0);
    if (!base) { printf("touch: no region to read below\n"); exit(2); }
    volatile char c = *((volatile char*)base - 1);
    (void)c;
    printf("touch: the read returned\n");
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && strcmp(argv[1], "touch-below") == 0) {
        pthread_t t;
        if (pthread_create(&t, 0, touch_below, 0) == 0) pthread_join(t, 0);
        return 0;
    }
    struct probe q;

    int rc = run(4u << 20, 3 * 1024, &q);
    printf("room: rc=%d told=%zu done=%d\n", rc, q.told, q.done);
    check(rc == 0 && q.done == 1, "a thread that asked for 4 MiB has the room");
    check(q.told >= (4u << 20), "and is told at least 4 MiB");

    rc = run(0, 6 * 1024, &q);
    printf("default: rc=%d told=%zu done=%d\n", rc, q.told, q.done);
    check(rc == 0 && q.done == 1, "a thread that asked for nothing has 6 MiB of room");

    const long before = resident_kib();
    rc = run(256u << 20, 1, &q);
    const long after = resident_kib();
    printf("resident: before=%ld after=%ld KiB rc=%d\n", before, after, rc);
    if (before < 0) printf("  not observed  the resident size could not be read\n");
    else check(rc == 0 && after - before < 32 * 1024, "asking for 256 MiB does not commit it");

    {
        static char region[1u << 20];
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstack(&attr, region, sizeof region);
        pthread_t t;
        rc = pthread_create(&t, &attr, body, &q);
        pthread_attr_destroy(&attr);
        printf("supplied: rc=%d\n", rc);
        if (rc == 0) pthread_join(t, 0);
        check(rc == EINVAL, "a stack the caller supplies is refused");
    }

    char* const args[] = { argv[0], "touch-below", 0 };
    pid_t child = -1;
    int status = 0;
    if (posix_spawn(&child, argv[0], 0, 0, args, environ) == 0 && waitpid(child, &status, 0) == child) {
        printf("guard: exited=%d status=%d\n", WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        check(!(WIFEXITED(status) && WEXITSTATUS(status) == 0), "a read below a thread's stack does not succeed");
        check(!(WIFEXITED(status) && WEXITSTATUS(status) == 2), "the thread was told a region to read below");
    } else {
        printf("  not observed  the program could not be started again\n");
    }

    /* DIAGNOSTIC, NOT ASSERTED: the same read in a program duplicated by fork,
     * which on one system was seen to return. */
    const pid_t dup = fork();
    if (dup == 0) {
        pthread_t t;
        const int crc = pthread_create(&t, 0, touch_below, 0);
        printf("diagnostic fork: pthread_create in the copy rc=%d\n", crc);
        if (crc == 0) pthread_join(t, 0);
        _exit(0);
    }
    if (dup > 0 && waitpid(dup, &status, 0) == dup)
        printf("diagnostic fork: exited=%d status=%d\n", WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    printf("-- failures: %d --\n", failures);
    return failures != 0;
}
