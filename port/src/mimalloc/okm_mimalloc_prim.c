/* mimalloc's primitives, on openkal (0.26.0, feature `malloc-mimalloc').
 *
 * mimalloc reaches its environment through one interface of its own
 * (`include/mimalloc/prim.h'), and upstream selects an implementation of it by
 * the preprocessor in `src/prim/prim.c' --- Windows, macOS, Wasm or Unix. This
 * library is none of those to the allocator: it is above openkal, and
 * `src/prim/prim.c' is excluded from the embedded sources (mcpp.toml) so that
 * this file is the implementation instead. Everything below is a call into
 * openkal, or into this C library's public surface.
 *
 * MEMORY. A range is reserved and backed part by part (`openkal.region'), as
 * mimalloc's own Windows and Unix layers do with VirtualAlloc and mmap; giving
 * back a part's memory is `kal_region_decommit', and letting its contents go is
 * `kal_region_discard'. A range is released only whole, so mimalloc is told it
 * cannot free a part (`has_partial_free'), as on Windows. Where the
 * implementation has no regions, memory is `kal_alloc' and is always backed:
 * mimalloc is told it cannot reserve without backing (`has_virtual_reserve'),
 * and the other operations have nothing to do.
 *
 * NOT SUPPORTED, AND ANSWERED SO: large and huge pages, and guard pages
 * (openkal states no protection, region.h); one NUMA node.
 *
 * WRITTEN AGAINST THE PUBLIC HEADERS. This file and the glue beside it are
 * compiled without musl's internal overlay (mcpp.toml, `OKM_MUSL_INTERNAL'),
 * because mimalloc's headers use names that overlay takes. */
#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/prim.h"
#include "mimalloc/heap/process.h"

#include <openkal.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../okm_stat.h"

/* THE SNAPSHOT'S RECORD (port/src/okm.h, whose internal names this file does
 * not take): while a runner runs the initialisers for a snapshot (phase 1)
 * nothing is asked of the environment but memory --- no randomness, no clock,
 * no stream --- and in a program started from one (phase 2) the arena's memory
 * is never given back, nor its backing taken. */
struct okm_snapshot { kal_u64 arena, size; kal_u32 phase, reserved; kal_u64 context; };
extern struct okm_snapshot __okrun_snapshot;
static int sealing(void) { return __okrun_snapshot.phase == 1; }
static int from_snapshot(const void* p)
{
	const kal_u64 a = (kal_u64)(uintptr_t)p;
	return __okrun_snapshot.phase == 2 && a >= __okrun_snapshot.arena
	    && a - __okrun_snapshot.arena < __okrun_snapshot.size;
}

extern __typeof(kal_region_reserve)  kal_region_reserve  __attribute__((__weak__));
extern __typeof(kal_region_commit)   kal_region_commit   __attribute__((__weak__));
extern __typeof(kal_region_decommit) kal_region_decommit __attribute__((__weak__));
extern __typeof(kal_region_discard)  kal_region_discard  __attribute__((__weak__));
extern __typeof(kal_region_release)  kal_region_release  __attribute__((__weak__));
extern __typeof(kal_region_props)    kal_region_props    __attribute__((__weak__));
extern __typeof(kal_random_fill)     kal_random_fill     __attribute__((__weak__));
extern __typeof(kal_process_self)    kal_process_self    __attribute__((__weak__));
extern __typeof(kal_process_info)    kal_process_info    __attribute__((__weak__));
extern __typeof(kal_process_close)   kal_process_close   __attribute__((__weak__));

static int g_regions;      /* the implementation provides `openkal.region' */
static size_t g_grain;     /* the granularity every request is a multiple of */

static int regions(void)
{
	return kal_region_reserve && kal_region_commit && kal_region_decommit
	    && kal_region_discard && kal_region_release && kal_region_props;
}

void _mi_prim_mem_init(mi_os_mem_config_t* config)
{
	/* openkal's granularity, no smaller than 4096 and a power of two: the
	 * page this C library reports (okm_start.c), and the unit of every region
	 * operation. */
	kal_uintptr g = kal_memory_granularity();
	if (g < 4096) g = 4096;
	else if (g & (g - 1)) {
		kal_uintptr p = 4096;
		while (p && p < g) p <<= 1;
		g = p ? p : 4096;
	}
	g_grain = (size_t)g;
	g_regions = regions();
#if defined(__APPLE__) && defined(__aarch64__)
	/* KEY ZERO IS mimalloc'S INVALID KEY ON THIS PLATFORM (atomic.h,
	 * MI_PTHREAD_KEY_INVALID), because Apple's own library never hands it out.
	 * musl hands out the lowest free key, and the allocator's keys are the
	 * first a program creates --- just after this, in the allocator's start;
	 * given zero, it would find no heap on any thread and start one on every
	 * call. So zero is taken here, once, and kept. */
	pthread_key_t k;
	if (pthread_key_create(&k, 0) == 0 && k != 0) pthread_key_delete(k);
#endif
	/* WITHOUT REGIONS AN ARENA IS BACKED WHEN IT IS RESERVED. Where it cannot
	 * reserve without backing, mimalloc reserves a quarter of its arena size
	 * (1 GiB on a 64-bit machine), and falls back to 128 MiB. A firmware pool
	 * does not hold that twice: measured under OVMF with 512 MiB, a carrier's
	 * allocator and its image's each reserve one, the second and its fallback
	 * failed, and every allocation the first arena did not hold failed with
	 * them. So an arena here is the smallest mimalloc admits
	 * (MI_ARENA_MIN_SIZE, after the quarter); a larger request reserves an
	 * arena of its own size, and later arenas grow as mimalloc grows them. A
	 * size the program states (MIMALLOC_ARENA_RESERVE) is kept. */
	if (!g_regions) {
		char stated[64];
		if (_mi_prim_getenv("MIMALLOC_ARENA_RESERVE", stated, sizeof stated) != 1)
			mi_option_set(mi_option_arena_reserve, (long)(4 * MI_ARENA_MIN_SIZE / MI_KiB));
	}
	config->page_size = g_grain;
	config->alloc_granularity = g_grain;
	config->large_page_size = 0;
	config->has_partial_free = false;
	config->has_virtual_reserve = g_regions != 0;
	config->has_overcommit = g_regions && (kal_region_props() & KAL_REGION_PROP_OVERCOMMIT) != 0;
	config->has_transparent_huge_pages = false;
}

static size_t whole(size_t n) { return (n + g_grain - 1) & ~(g_grain - 1); }

/* THE ALIGNMENT A PART WAS TAKEN AT, without regions. `kal_free' is told the
 * alignment `kal_alloc' was, and mimalloc frees with the address and the size
 * only; a part taken at an alignment other than the grain is recorded here
 * until it is freed. There are few: mimalloc states an alignment for an arena
 * (256 MiB, where its page information is aligned) and for a block too large
 * for one. */
struct okm_aligned_part {
	void *addr;
	size_t align;
	struct okm_aligned_part *next;
};
static struct okm_aligned_part *g_aligned_parts;
static char g_aligned_lock;

static void aligned_parts_lock(void)
{
	while (__atomic_test_and_set(&g_aligned_lock, __ATOMIC_ACQUIRE)) sched_yield();
}
static void aligned_parts_unlock(void) { __atomic_clear(&g_aligned_lock, __ATOMIC_RELEASE); }

/* Memory of `size' at `align', recorded where `align' is not the grain. An
 * implementation that cannot place it at that alignment is asked at the grain,
 * and mimalloc then aligns within a larger part, as it does wherever an
 * alignment is not honoured. */
static void *backed_part(size_t size, size_t align)
{
	if (align != g_grain) {
		struct okm_aligned_part *part = kal_alloc(sizeof *part, _Alignof(struct okm_aligned_part));
		void *p = part ? kal_alloc(size, align) : 0;
		if (p) {
			part->addr = p;
			part->align = align;
			aligned_parts_lock();
			part->next = g_aligned_parts;
			g_aligned_parts = part;
			aligned_parts_unlock();
			return p;
		}
		if (part) kal_free(part, sizeof *part, _Alignof(struct okm_aligned_part));
	}
	return kal_alloc(size, g_grain);
}

static void backed_part_free(void *addr, size_t size)
{
	struct okm_aligned_part *found = 0;
	aligned_parts_lock();
	for (struct okm_aligned_part **at = &g_aligned_parts; *at; at = &(*at)->next)
		if ((*at)->addr == addr) {
			found = *at;
			*at = found->next;
			break;
		}
	aligned_parts_unlock();
	if (!found) {
		kal_free(addr, size, g_grain);
		return;
	}
	kal_free(addr, size, found->align);
	kal_free(found, sizeof *found, _Alignof(struct okm_aligned_part));
}

int _mi_prim_alloc(void* hint_addr, size_t size, size_t try_alignment, bool commit,
                   bool allow_large, bool* is_large, bool* is_zero, void** addr)
{
	(void)hint_addr; (void)allow_large;
	*is_large = false;
	size = whole(size);
	/* The alignment is honoured exactly where the implementation can: a region
	 * is reserved at one, and so is a part of memory that is backed at once. */
	size_t align = g_grain;
	if (try_alignment > align && (try_alignment & (try_alignment - 1)) == 0) align = try_alignment;
	if (g_regions) {
		*addr = kal_region_reserve(size, align, commit ? KAL_REGION_COMMIT : 0);
		*is_zero = true;
		if (!*addr) return ENOMEM;
		okm_stat_add(OKM_STAT_REGION_RESERVES, 1);
		okm_stat_add(OKM_STAT_REGION_RESERVED, size);
		return 0;
	}
	/* Backed at once and of unstated contents. A request to commit receives
	 * them as they are, and mimalloc clears what it must. A request NOT to
	 * commit is a reservation, which mimalloc reads as memory that is zero
	 * when it is first committed --- its page map commits parts of one and
	 * stores into them unread --- and nothing here commits later
	 * (`_mi_prim_commit' has nothing to do), so the reservation is cleared
	 * now. Measured under OVMF, whose pool is filled with a pattern: the
	 * page map's entries were the pattern, and the first page registered
	 * faulted. */
	*addr = backed_part(size, align);
	if (!*addr) return ENOMEM;
	if (!commit) memset(*addr, 0, size);
	*is_zero = !commit;
	return 0;
}

int _mi_prim_free(void* addr, size_t size)
{
	if (size == 0 || from_snapshot(addr)) return 0;
	size = whole(size);
	if (g_regions) {
		kal_region_release(addr, size);
		okm_stat_add(OKM_STAT_REGION_RELEASES, 1);
		okm_stat_sub(OKM_STAT_REGION_RESERVED, size);
	}
	else backed_part_free(addr, size);
	return 0;
}

int _mi_prim_commit(void* addr, size_t size, bool* is_zero)
{
	/* A part backed before keeps its contents (region.h), so nothing is
	 * promised about zero. */
	*is_zero = false;
	if (!g_regions || from_snapshot(addr)) return 0;
	okm_stat_add(OKM_STAT_REGION_COMMITS, 1);
	const int e = kal_region_commit(addr, size);
	return e == kal_ok ? 0 : ENOMEM;
}

int _mi_prim_decommit(void* addr, size_t size, bool* needs_recommit)
{
	if (!g_regions || from_snapshot(addr)) { *needs_recommit = false; return 0; }
	*needs_recommit = true;
	okm_stat_add(OKM_STAT_REGION_DECOMMITS, 1);
	return kal_region_decommit(addr, size) == kal_ok ? 0 : EINVAL;
}

int _mi_prim_reset(void* addr, size_t size)
{
	if (!g_regions || from_snapshot(addr)) return 0;
	okm_stat_add(OKM_STAT_REGION_DISCARDS, 1);
	return kal_region_discard(addr, size) == kal_ok ? 0 : EINVAL;
}

int _mi_prim_reuse(void* addr, size_t size)
{
	(void)addr; (void)size;
	return 0;
}

int _mi_prim_guard(void* addr, size_t size, bool set, bool is_committed)
{
	(void)addr; (void)size; (void)set; (void)is_committed;
	return ENOSYS;
}

int _mi_prim_alloc_huge_os_pages(void* hint_addr, size_t size, int numa_node,
                                 bool* is_zero, void** addr)
{
	(void)hint_addr; (void)size; (void)numa_node;
	*is_zero = false;
	*addr = NULL;
	return ENOMEM;
}

size_t _mi_prim_numa_node(void) { return 0; }
size_t _mi_prim_numa_node_count(void) { return 1; }

mi_msecs_t _mi_prim_clock_now(void)
{
	if (sealing()) return 0;
	return (mi_msecs_t)(kal_time_monotonic() / 1000000u);
}

/* What the implementation reports of this program: its processor time and the
 * memory it occupies (KAL_PINFO_CPU, KAL_PINFO_MEMORY). The processor time is
 * not divided into user and system time by openkal, and is reported as user
 * time. */
void _mi_prim_process_info(mi_process_info_t* pinfo)
{
	if (sealing() || !kal_process_self || !kal_process_info || !kal_process_close) return;
	struct kal_process me;
	if (kal_process_self(&me) != kal_ok) return;
	struct kal_program_info info = { .self_size = sizeof info };
	if (kal_process_info(me, KAL_PINFO_CPU | KAL_PINFO_MEMORY, &info) == kal_ok) {
		if (info.present & KAL_PINFO_CPU) pinfo->utime = (mi_msecs_t)(info.cpu_ns / 1000000u);
		if (info.present & KAL_PINFO_MEMORY) {
			pinfo->current_rss = (size_t)info.memory_bytes;
			pinfo->peak_rss    = (size_t)info.memory_peak_bytes;
		}
	}
	kal_process_close(me);
}

void _mi_prim_out_stderr(const char* msg)
{
	if (sealing()) return;
	kal_uintptr n = 0;
	while (msg[n]) n++;
	(void)kal_stream_write(kal_stderr(), msg, n);
}

/* The named values, read from this library's own copy of them: the allocator
 * reads its options while the C library starts (okm_start.c), after the copy
 * is made. Before it is made the answer is "not yet", and mimalloc asks again. */
extern char** __environ;

/* WHETHER THE SET HOLDS ANY OF MIMALLOC'S OWN (0.27.0): mimalloc asks for each
 * of its options by name, a pass over the whole set each time, and a program
 * that sets none of them --- every program, nearly --- is answered from one pass.
 * Asked again when the set is another (musl's setenv replaces the array). */
static char** g_scanned;
static int    g_has_own;

int _mi_prim_getenv(const char* name, char* result, size_t result_size)
{
	if (name == NULL) return -1;
	const size_t len = _mi_strlen(name);
	if (len == 0) return -1;
	char** env = __environ;
	if (env == NULL) return -1;
	if (env != g_scanned) {
		g_has_own = 0;
		for (int i = 0; i < 10000 && env[i] != NULL && !g_has_own; i++)
			g_has_own = _mi_strnicmp(env[i], "MIMALLOC_", 9) == 0;
		g_scanned = env;
	}
	if (!g_has_own) return 0;
	for (int i = 0; i < 10000 && env[i] != NULL; i++) {
		const char* s = env[i];
		if (_mi_strnicmp(name, s, len) == 0 && s[len] == '=') {
			if (!_mi_strlcpy(result, s + len + 1, result_size)) return -1;
			return 1;
		}
	}
	return 0;
}

bool _mi_prim_random_buf(void* buf, size_t buf_len)
{
	/* The weak randomness mimalloc falls back on while a snapshot's
	 * initialisers run is made strong when a program starts from it
	 * (__okm_malloc_start). */
	if (sealing() || !kal_random_fill) return false;
	return kal_random_fill(buf, buf_len) == kal_ok;
}

/* THE END OF A THREAD, through a key of the thread's record whose destructor
 * musl runs when the thread ends --- upstream's own arrangement for a C library
 * with POSIX threads. The thread's heap is given back there. */
static _Atomic(pthread_key_t) g_done_key = MI_ATOMIC_VAR_INIT(MI_PTHREAD_KEY_INVALID);

static void thread_ends(void* value)
{
	if (value != NULL) _mi_thread_done((mi_theap_t*)value);
}

void _mi_prim_thread_init_auto_done(void)
{
	pthread_key_t key;
	if (pthread_key_create(&key, &thread_ends) == 0)
		mi_atomic_store_release(&g_done_key, key);
}

void _mi_prim_thread_done_auto_done(void)
{
	pthread_key_t key = mi_atomic_exchange_relaxed(&g_done_key, MI_PTHREAD_KEY_INVALID);
	if (key != MI_PTHREAD_KEY_INVALID) pthread_key_delete(key);
}

void _mi_prim_thread_associate_default_theap(mi_theap_t* theap)
{
	pthread_key_t key = mi_atomic_load_relaxed(&g_done_key);
	if (key != MI_PTHREAD_KEY_INVALID) pthread_setspecific(key, theap);
}

bool _mi_prim_thread_is_in_threadpool(void) { return false; }

void _mi_prim_thread_yield(void) { sched_yield(); }

bool _mi_prim_process_is_killed(void) { return false; }

/* What upstream's `src/prim/prim.c' defines for every system, which is excluded
 * with the rest of that file. */
bool _mi_is_redirected(void) { return false; }

bool _mi_allocator_init(const char** message)
{
	if (message != NULL) *message = NULL;
	return true;
}

void _mi_allocator_done(void) {}

/* THE PROCESS'S START AND END. Upstream starts the allocator from a constructor
 * of priority 101. Here it is started by the C library (okm_start.c,
 * `__okm_malloc_init'), at the end of the library's own start: on one object
 * format initialisers run before the program's entry and in the order their
 * objects were linked (port/src/mach), so a constructor could run before the
 * C library it calls is up. The end is a destructor, which runs from `exit'. */
/* WHAT THE ALLOCATOR KEEPS, BY THE PROGRAM'S RESOURCE PREFERENCE (0.27.0;
 * openkal 0.23, mcpp design 2026-11-03 part 4 §16): memory a heap no longer
 * uses is given back after mimalloc's own delay (10 ms) when balanced, after a
 * second for performance, and at once for economy. A default, which
 * MIMALLOC_PURGE_DELAY states otherwise. Weak: a runner of openkal 0.22 has
 * no preference to tell. */
extern __typeof(kal_env_resources) kal_env_resources __attribute__((__weak__));

static void okm_malloc_preference(void)
{
	if (!kal_env_resources) return;
	switch (kal_env_resources()) {
	case KAL_RESOURCES_PERFORMANCE: mi_option_set_default(mi_option_purge_delay, 1000); break;
	case KAL_RESOURCES_ECONOMY:     mi_option_set_default(mi_option_purge_delay, 0); break;
	default: break;
	}
}

/* AN ARENA'S SIZE, IN KiB (0.29.6). With regions, mimalloc's own: a gibibyte
 * reserved and backed part by part. Without them memory is `kal_alloc' and is
 * backed whole at once, which is a firmware's pool on a machine of some hundred
 * mebibytes: an arena is then 16 MiB (mimalloc takes a quarter of it, 4 MiB,
 * where it cannot reserve without backing, arena.c) and grows as mimalloc grows
 * it, by doubling every eighth arena. */
static long okm_arena_reserve(void)
{
	return regions() ? 1024 * 1024 : 16 * 1024;
}

__attribute__((__visibility__("hidden"))) void __okm_malloc_init(void)
{
	/* Not while a snapshot's initialisers run: the start supplies it (below). */
	if (!sealing()) okm_malloc_preference();
	/* A SNAPSHOT'S ARENA HOLDS WHAT IS RESERVED WHILE ITS INITIALISERS RUN, and a
	 * program started from it reserves all of it again: mimalloc reserves a
	 * gibibyte at a time, so it is asked for little while they run. */
	if (sealing()) mi_option_set(mi_option_arena_reserve, 4 * 1024);   /* KiB */
	else mi_option_set_default(mi_option_arena_reserve, okm_arena_reserve());
	_mi_auto_process_init();
}

/* A PROGRAM STARTED FROM A SNAPSHOT (0.27.0, okm_start.c): the randomness the
 * initialisers ran without is drawn now, as mimalloc draws it again where it
 * started weakly, and the arena is reserved at its own size again. */
__attribute__((__visibility__("hidden"))) void __okm_malloc_start(void)
{
	mi_option_set(mi_option_arena_reserve, okm_arena_reserve());
	okm_malloc_preference();
	mi_theap_t* theap = _mi_theap_default();
	if (theap != NULL) {
		_mi_random_reinit_if_weak(&theap->random);
		mi_subproc_t* subproc = mi_theap_subproc(theap);
		if (subproc->theap_meta != NULL) {
			mi_lock(&subproc->theap_meta_lock) {
				_mi_random_reinit_if_weak(&subproc->theap_meta->random);
			}
		}
	}
}

__attribute__((__destructor__)) static void okm_malloc_done(void)
{
	_mi_auto_process_done();
}
