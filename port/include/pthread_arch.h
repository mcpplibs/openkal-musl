/* The thread pointer.
 *
 * musl obtains the current execution context's state by reading a register the
 * architecture reserves for it. openkal declines to own that register: the
 * convention belongs to openarch, and an operation here that installed a thread
 * pointer would place a processor's calling convention in an interface written
 * to be independent of one.
 *
 * The substitute is not a variable declared thread_local. port/src/okm_context.c
 * records why at length, and the short form is that on one of the toolchains
 * this package is built by, reaching such a variable calls a helper that
 * allocates --- and the allocator is what this library is providing, so the
 * dependency is circular and appears only as a program that never returns.
 *
 * What is used instead is a table keyed on the identity openkal already gives
 * every context. It costs a lookup where a load would have done, and it makes
 * this port independent of whether an implementation's contexts have
 * thread-local storage at all.
 */
#ifndef OKM_PTHREAD_ARCH_H
#define OKM_PTHREAD_ARCH_H

uintptr_t __okm_get_tp(void);

static inline uintptr_t __get_tp(void) { return __okm_get_tp(); }

/* musl reads MC_PC only inside its cancellation signal handler. openkal has no
 * asynchronous delivery, so no such handler is installed and none runs; the
 * definition exists because the sources that mention it must still compile, and
 * it names a member that does exist so that a mistake elsewhere is still
 * diagnosed rather than swallowed by a name nothing has. */
#if defined(__aarch64__)
#define MC_PC pc
#elif defined(__riscv) && __riscv_xlen == 64
/* That architecture spells the array with two leading underscores, and the
 * definition here names the member that exists rather than one that does not
 * --- which is the whole point of the paragraph above. */
#define MC_PC __gregs[0]
#else
#define MC_PC gregs[0]
#endif

#endif
