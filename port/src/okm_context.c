/* Where a C library keeps the state that belongs to one execution context.
 *
 * musl reaches its per-context state --- its error value, its locale, its
 * cancellation state --- through one pointer, and on the systems it was written
 * for that pointer is in a register the architecture reserves. openkal declines
 * to own that register: the convention belongs to openarch, and an operation
 * here that installed a thread pointer would put a processor's calling
 * convention into an interface written to be independent of one.
 *
 * The obvious substitute is a variable declared thread_local, and openkal even
 * reports whether a started context observes one --- KAL_TASK_PROP_THREAD_LOCAL,
 * clause 7.10. This port does not use it, and the reason is worth recording
 * because it was found by running rather than by reading.
 *
 * On one of the three toolchains this package is built by, a thread_local
 * variable is not reached through an address the processor supplies. It is
 * reached through a helper the compiler emits a call to, and that helper
 * allocates the storage on first use --- it calls malloc. malloc is this
 * library's. This library's malloc touches its error value. Its error value is
 * per-context state, reached through the thread pointer, reached through the
 * helper, which allocates.
 *
 * The recursion is unbounded, it appears only at run time as a program that
 * starts and never returns, and neither side of it reads as though anything is
 * wrong. A C library cannot keep the state it needs in order to allocate in a
 * place that must be allocated.
 *
 * So the state is kept here, keyed on the identity openkal already gives every
 * context. The cost is a lookup where a load would have done; the benefit is
 * that this port depends on nothing beyond openkal --- it runs above an
 * implementation whose contexts have no thread-local storage at all, which is
 * an arrangement clause 7.10 explicitly permits.
 */
#include "okm.h"

#include <stdint.h>

/* One entry per live context. The bound is fixed and exhaustion is refused
 * rather than grown around: growing it would mean allocating, and this table is
 * what the allocator needs in order to run. */
#define OKM_CONTEXTS 512

struct okm_slot {
	volatile kal_uintptr key;    /* 0 free, ~0 retired, else kal_task_current() */
	uintptr_t            tp;     /* what musl's __get_tp() answers */
	void*                self;   /* what this port's own thread record is */
};

static struct okm_slot g_slots[OKM_CONTEXTS];

#define OKM_RETIRED ((kal_uintptr)-1)

static unsigned start_at(kal_uintptr key)
{
	/* Any dispersal will do: the identities an implementation hands out are
	 * consecutive on one and pointer-shaped on another, and a table indexed by
	 * either directly would cluster on one of them. */
	kal_uintptr h = key * (kal_uintptr)0x9E3779B97F4A7C15ull;
	return (unsigned)((h >> 32) & (OKM_CONTEXTS - 1));
}

static struct okm_slot* find(kal_uintptr me, int create)
{
	unsigned i = start_at(me);
	struct okm_slot* free_slot = 0;
	for (unsigned n = 0; n < OKM_CONTEXTS; n++, i = (i + 1) & (OKM_CONTEXTS - 1)) {
		const kal_uintptr k = __atomic_load_n(&g_slots[i].key, __ATOMIC_ACQUIRE);
		if (k == me) return &g_slots[i];
		if (k == 0) {
			/* The end of this probe sequence: nothing beyond it can hold this
			 * key, so an entry that is looked up and not found stops here. */
			if (!create) return 0;
			if (!free_slot) free_slot = &g_slots[i];
			break;
		}
		if (k == OKM_RETIRED && !free_slot) free_slot = &g_slots[i];
	}
	if (!create) return 0;
	if (!free_slot) return 0;
	kal_uintptr expected = __atomic_load_n(&free_slot->key, __ATOMIC_RELAXED);
	if (expected == me) return free_slot;
	if (expected != 0 && expected != OKM_RETIRED) return 0;
	if (!__atomic_compare_exchange_n(&free_slot->key, &expected, me, 0,
	                                 __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		return 0;
	return free_slot;
}

uintptr_t __okm_get_tp(void)
{
	struct okm_slot* s = find(kal_task_current(), 0);
	return s ? s->tp : 0;
}

void __okm_set_tp(uintptr_t value)
{
	struct okm_slot* s = find(kal_task_current(), 1);
	if (!s) {
		static const char m[] =
			"openkal-musl: more execution contexts than this port can record\n";
		kal_abort(m, sizeof m - 1);
	}
	s->tp = value;
}

void* __okm_get_self(void)
{
	struct okm_slot* s = find(kal_task_current(), 0);
	return s ? s->self : 0;
}

void __okm_set_self(void* value)
{
	struct okm_slot* s = find(kal_task_current(), 1);
	if (s) s->self = value;
}

/* Called as the last thing a context does, so that the identity may be reused.
 * An entry is retired rather than freed: a later lookup must be able to walk
 * past it to reach one placed beyond it by an earlier collision. */
void __okm_release_context(void)
{
	struct okm_slot* s = find(kal_task_current(), 0);
	if (!s) return;
	s->tp = 0;
	s->self = 0;
	__atomic_store_n(&s->key, OKM_RETIRED, __ATOMIC_RELEASE);
}
