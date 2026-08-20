/* The seam.
 *
 * musl reaches its kernel through seven inline functions declared once per
 * architecture. Replacing that architecture's copy of this header is the whole
 * of the redirection: 149 of musl's sources issue system calls and every one of
 * them does so through these seven, so the port touches one header rather than
 * a hundred and forty-nine files.
 *
 * The header is found before musl's own because the directory containing it is
 * searched first. Nothing in musl is edited to make that happen.
 */
#ifndef OKM_SYSCALL_ARCH_H
#define OKM_SYSCALL_ARCH_H

/* The width of a system call's argument.
 *
 * musl states this per architecture and provides the hook, because two of its
 * own targets already need it: x32 and the n32 ABI of mips64 both pass
 * arguments wider than a `long'. The hook is used here for the opposite reason
 * --- a `long' that is narrower than a pointer rather than wider than an
 * argument --- and the mechanism is the same.
 *
 * On every architecture musl itself supports, a `long' holds a pointer. On
 * Windows it does not: that data model has thirty-two-bit `long' and
 * sixty-four-bit pointers, and an argument passed as a `long' would lose half
 * of every address. The type is therefore derived from the compiler rather than
 * assumed, and on a target where the two agree these definitions are what musl
 * would have used anyway.
 */
#if defined(__SIZEOF_LONG__) && defined(__SIZEOF_POINTER__) \
    && __SIZEOF_LONG__ < __SIZEOF_POINTER__
typedef long long          syscall_arg_t;
typedef unsigned long long syscall_uret_t;
#define __scc(X) ((long long)(X))
#else
typedef long          syscall_arg_t;
typedef unsigned long syscall_uret_t;
#define __scc(X) ((long)(X))
#endif

/* Both data models this port builds for pass every argument in one word and
 * split none across two. The definitions exist because musl's generic code
 * refers to them. */
#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

syscall_arg_t __okm_syscall(syscall_arg_t n, syscall_arg_t a1, syscall_arg_t a2,
                            syscall_arg_t a3, syscall_arg_t a4, syscall_arg_t a5,
                            syscall_arg_t a6);

static __inline syscall_arg_t __syscall0(syscall_arg_t n)
{ return __okm_syscall(n, 0, 0, 0, 0, 0, 0); }
static __inline syscall_arg_t __syscall1(syscall_arg_t n, syscall_arg_t a1)
{ return __okm_syscall(n, a1, 0, 0, 0, 0, 0); }
static __inline syscall_arg_t __syscall2(syscall_arg_t n, syscall_arg_t a1, syscall_arg_t a2)
{ return __okm_syscall(n, a1, a2, 0, 0, 0, 0); }
static __inline syscall_arg_t __syscall3(syscall_arg_t n, syscall_arg_t a1, syscall_arg_t a2,
                                         syscall_arg_t a3)
{ return __okm_syscall(n, a1, a2, a3, 0, 0, 0); }
static __inline syscall_arg_t __syscall4(syscall_arg_t n, syscall_arg_t a1, syscall_arg_t a2,
                                         syscall_arg_t a3, syscall_arg_t a4)
{ return __okm_syscall(n, a1, a2, a3, a4, 0, 0); }
static __inline syscall_arg_t __syscall5(syscall_arg_t n, syscall_arg_t a1, syscall_arg_t a2,
                                         syscall_arg_t a3, syscall_arg_t a4, syscall_arg_t a5)
{ return __okm_syscall(n, a1, a2, a3, a4, a5, 0); }
static __inline syscall_arg_t __syscall6(syscall_arg_t n, syscall_arg_t a1, syscall_arg_t a2,
                                         syscall_arg_t a3, syscall_arg_t a4, syscall_arg_t a5,
                                         syscall_arg_t a6)
{ return __okm_syscall(n, a1, a2, a3, a4, a5, a6); }

#endif
