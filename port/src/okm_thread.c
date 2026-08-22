/* Execution contexts, and the primitive they are built upon.
 *
 * This is the one place where the correspondence is not a translation.
 *
 * musl creates a thread with `__clone', whose caller supplies the stack, the
 * thread pointer and a set of sharing flags, and requires the created context
 * to begin with the thread pointer it was given. openkal offers
 * kal_task_start, which supplies a whole execution context: the stack is the
 * implementation's, and so is whatever register names the context. The two
 * cannot express each other, and the specification says why --- an environment
 * that does not allocate stacks separately cannot honour a request for one.
 *
 * musl anticipates this. `src/thread/clone.c' is a generic stub returning
 * -ENOSYS, present so that a target without `clone' can be built. This file
 * replaces that stub with an implementation upon kal_task_start rather than
 * altering pthread_create, so the only thing musl loses is the ability to
 * choose the stack --- and the stack it would have chosen is unused rather
 * than absent, which costs memory and no correctness.
 *
 * What the caller does still get is the thread pointer it asked for: the
 * created context assigns it to __okm_tp before running anything, which is
 * exactly what CLONE_SETTLS would have done.
 */
#include "okm.h"
#include "okm_opt.h"

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "futex.h"

void      __okm_set_tp(uintptr_t value);
void*     __okm_get_self(void);
void      __okm_set_self(void* value);
void      __okm_release_context(void);

struct okm_thread {
	int  (*fn)(void*);
	void*  arg;
	void*  tls;
	volatile int* ctid;
	int    tid;
	struct kal_task task;
	jmp_buf back;                 /* where SYS_exit returns to */
	volatile int finished;
	struct okm_thread* next;
};

/* The context this one is, when it is not the first. The first context has no
 * such record, and the difference is what SYS_exit consults. It is kept beside
 * the thread pointer, in the same table and for the same reason. */

/* Contexts that have finished and whose openkal handle has not yet been
 * released. openkal requires that a context be waited for at most once and
 * that it be waited for at all --- the implementation reclaims the stack it
 * supplied when the wait returns --- while POSIX lets a detached thread be
 * forgotten. The two are reconciled by waiting later rather than never. */
static struct okm_thread* okm_done;

static int g_tid = 1;

static void reap(void)
{
	okm_lock();
	struct okm_thread** p = &okm_done;
	while (*p) {
		struct okm_thread* t = *p;
		if (!__atomic_load_n(&t->finished, __ATOMIC_ACQUIRE)) { p = &t->next; continue; }
		*p = t->next;
		okm_unlock();
		okm_task_join(t->task);
		kal_free(t, sizeof *t, _Alignof(struct okm_thread));
		okm_lock();
		p = &okm_done;
	}
	okm_unlock();
}

static void entry(void* p)
{
	struct okm_thread* t = p;
	__okm_set_tp((uintptr_t)t->tls);
	__okm_set_self(t);

	/* musl ends a thread by issuing SYS_exit, which does not return. There is
	 * no openkal operation that ends a context from within, and there does not
	 * need to be: returning from the function ends it. The jump is how a call
	 * that does not return becomes a return. */
	if (setjmp(t->back) == 0) t->fn(t->arg);

	/* The word the creating context is waiting upon. Linux clears it in the
	 * kernel after the thread has gone; here the thread clears it as the last
	 * thing it does, which is the same order. */
	if (t->ctid) {
		__atomic_store_n((int*)t->ctid, 0, __ATOMIC_RELEASE);
		kal_uintptr woken = 0;
		okm_task_wake((const kal_u32*)t->ctid, 1, &woken);
	}
	__atomic_store_n(&t->finished, 1, __ATOMIC_RELEASE);

	/* The identity this context was known by may now be given to another, so
	 * what was recorded against it is withdrawn before it can be. */
	__okm_release_context();
}

int __clone(int (*fn)(void*), void* stack, int flags, void* arg, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, arg);
	int*  ptid = __builtin_va_arg(ap, int*);
	void* tls  = __builtin_va_arg(ap, void*);
	int*  ctid = __builtin_va_arg(ap, int*);
	__builtin_va_end(ap);
	(void)stack; (void)flags;

	reap();

	struct okm_thread* t = kal_alloc(sizeof *t, _Alignof(struct okm_thread));
	if (!t) return -EAGAIN;
	memset(t, 0, sizeof *t);
	t->fn = fn; t->arg = arg; t->tls = tls; t->ctid = ctid;

	/* The identity musl records for the context. Linux supplies it through
	 * CLONE_PARENT_SETTID, before the system call returns, because the creating
	 * context uses it immediately; the same ordering is obtained by choosing it
	 * here rather than letting the created context report it. */
	okm_lock();
	t->tid = ++g_tid;
	okm_unlock();
	if (ptid) *ptid = t->tid;

	if (okm_task_start(entry, t, &t->task) != kal_ok) {
		kal_free(t, sizeof *t, _Alignof(struct okm_thread));
		return -EAGAIN;
	}

	okm_lock();
	t->next = okm_done;
	okm_done = t;
	okm_unlock();
	return t->tid;
}

syscall_arg_t __okm_task_exit(int code)
{
	struct okm_thread* self = (struct okm_thread*)__okm_get_self();
	if (self) longjmp(self->back, 1);
	/* The first context. Ending it ends the program, which is what the caller
	 * asked for and what Linux would have done. */
	kal_exit(code);
	return 0;
}

/* --- the suspension primitive ------------------------------------------------ */

syscall_arg_t __okm_futex(const int* addr, int op, int val, const struct timespec* t)
{
	op &= ~(FUTEX_PRIVATE | 256 /* FUTEX_CLOCK_REALTIME */);
	switch (op) {
	case FUTEX_WAIT: {
		const kal_u64 ns = t
			? (kal_u64)t->tv_sec * 1000000000u + (kal_u64)t->tv_nsec
			: 0;
		const int e = okm_task_wait((const kal_u32*)addr, (kal_u32)val, ns);
		if (e == kal_ok) return 0;
		if (e == kal_err_again) return -ETIMEDOUT;
		return -okm_errno(e);
	}
	case FUTEX_WAKE: {
		kal_uintptr woken = 0;
		const int e = okm_task_wake((const kal_u32*)addr,
		                            val < 0 ? (kal_uintptr)-1 : (kal_uintptr)val, &woken);
		if (e != kal_ok) return -okm_errno(e);
		return (long)woken;
	}
	default:
		return -ENOSYS;
	}
}

/* --- cancellation ------------------------------------------------------------- */

/* musl implements deferred cancellation by delivering a signal and comparing
 * the interrupted program counter against three labels inside a hand-written
 * system call sequence. openkal has no asynchronous delivery, so no signal is
 * ever delivered and the comparison never happens; what remains of the
 * mechanism is the part that works without one, which is the check a
 * cancellation point makes before it blocks. */
hidden syscall_arg_t __cancel(void);

__attribute__((__visibility__("hidden"))) const char __cp_begin[1]  = { 0 };
__attribute__((__visibility__("hidden"))) const char __cp_end[1]    = { 0 };
__attribute__((__visibility__("hidden"))) const char __cp_cancel[1] = { 0 };

syscall_arg_t __syscall_cp_asm(volatile void* cancel, syscall_arg_t nr,
                              syscall_arg_t a, syscall_arg_t b, syscall_arg_t c,
                              syscall_arg_t d, syscall_arg_t e, syscall_arg_t f)
{
	if (*(volatile int*)cancel) return __cancel();
	return __okm_syscall(nr, a, b, c, d, e, f);
}
