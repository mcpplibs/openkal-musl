/* The thread pointer.
 *
 * musl obtains the current execution context's state by reading a register the
 * architecture reserves for it. openkal declines to own that register: the
 * convention belongs to openarch, and an operation here that installed a
 * thread pointer would place a processor's calling convention in an interface
 * written to be independent of one.
 *
 * What openkal reports instead is whether a context started by kal_task_start
 * observes the thread-local storage of the toolchain that compiled the
 * program --- KAL_TASK_PROP_THREAD_LOCAL, clause 7.10. This port requires that
 * property and keeps musl's per-context pointer in one such variable. The
 * requirement is checked at startup rather than assumed, because an
 * implementation that lacks the property would otherwise produce a program in
 * which every context shares one library state.
 *
 * The variable is declared with the GNU spelling rather than C11's
 * _Thread_local because musl's sources are compiled as C99.
 */
#ifndef OKM_PTHREAD_ARCH_H
#define OKM_PTHREAD_ARCH_H

extern __thread uintptr_t __okm_tp;

static inline uintptr_t __get_tp(void) { return __okm_tp; }

/* musl reads MC_PC only inside its cancellation signal handler. openkal has no
 * asynchronous delivery, so no such handler is installed and none runs; the
 * definition exists because the sources that mention it must still compile,
 * and it names a member that does exist so that the compiler's diagnosis of a
 * mistake elsewhere is not swallowed. */
#define MC_PC gregs[0]

#endif
