/* The C library's allocation names, answered by the embedded mimalloc (0.26.0,
 * feature `malloc-mimalloc').
 *
 * musl names its allocator through five internal names --- `__libc_malloc_impl',
 * `__libc_malloc', `__libc_realloc', `__libc_free' and `__libc_calloc' --- which
 * musl's own sources call when they must reach the C library's allocator rather
 * than one a program interposed, and through the public names a program calls.
 * mallocng defines the first and three public ones; lite_malloc.c, free.c,
 * realloc.c, calloc.c and libc_calloc.c define the rest by forwarding. Under
 * this feature those five sources are withheld (mcpp.toml) and every name is
 * defined here, each by one call into mimalloc.
 *
 * `malloc' IS WEAK, as musl's own is (lite_malloc.c), so that a program that
 * defines it replaces it; the others are strong, as musl's are.
 *
 * WHAT THE OTHER ALLOCATOR NEEDED AND THIS ONE DOES NOT. `__malloc_tc_flush'
 * gave mallocng's per-thread cache back when a thread ended; mimalloc gives a
 * thread's heap back through a key of the thread's record (okm_mimalloc_prim.c),
 * so the name is defined and does nothing. `__malloc_atfork' kept mallocng's
 * lock across a duplication; mimalloc registers its own handlers with
 * `pthread_atfork', and musl's default for the name (fork.c) stands. */
#include <stddef.h>
#include "mimalloc.h"
#include "../okm_stat.h"

#define OKM_HIDDEN __attribute__((__visibility__("hidden")))

static inline void count_malloc(size_t n)
{
	okm_stat_add(OKM_STAT_MALLOC_CALLS, 1);
	okm_stat_add(OKM_STAT_MALLOC_BYTES, n);
}

__attribute__((__weak__)) void* malloc(size_t n) { count_malloc(n); return mi_malloc(n); }

OKM_HIDDEN void* __libc_malloc_impl(size_t n) { count_malloc(n); return mi_malloc(n); }
OKM_HIDDEN void* __libc_malloc(size_t n)      { count_malloc(n); return mi_malloc(n); }

void free(void* p)
{
	okm_stat_add(OKM_STAT_FREE_CALLS, 1);
	mi_free(p);
}

OKM_HIDDEN void __libc_free(void* p)
{
	okm_stat_add(OKM_STAT_FREE_CALLS, 1);
	mi_free(p);
}

void* realloc(void* p, size_t n)                  { count_malloc(n); return mi_realloc(p, n); }
OKM_HIDDEN void* __libc_realloc(void* p, size_t n) { count_malloc(n); return mi_realloc(p, n); }

void* calloc(size_t m, size_t n)                  { count_malloc(m * n); return mi_calloc(m, n); }
OKM_HIDDEN void* __libc_calloc(size_t m, size_t n) { count_malloc(m * n); return mi_calloc(m, n); }

void* aligned_alloc(size_t align, size_t n) { count_malloc(n); return mi_aligned_alloc(align, n); }

size_t malloc_usable_size(void* p) { return p ? mi_usable_size(p) : 0; }

OKM_HIDDEN void __malloc_tc_flush(void) {}
