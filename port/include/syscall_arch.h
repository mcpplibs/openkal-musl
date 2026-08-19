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

/* Both architectures this port builds for are LP64, so a long carries every
 * argument and no argument is split across two. The definitions exist because
 * musl's generic code refers to them. */
#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

long __okm_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

static __inline long __syscall0(long n)
{ return __okm_syscall(n, 0, 0, 0, 0, 0, 0); }
static __inline long __syscall1(long n, long a1)
{ return __okm_syscall(n, a1, 0, 0, 0, 0, 0); }
static __inline long __syscall2(long n, long a1, long a2)
{ return __okm_syscall(n, a1, a2, 0, 0, 0, 0); }
static __inline long __syscall3(long n, long a1, long a2, long a3)
{ return __okm_syscall(n, a1, a2, a3, 0, 0, 0); }
static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{ return __okm_syscall(n, a1, a2, a3, a4, 0, 0); }
static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{ return __okm_syscall(n, a1, a2, a3, a4, a5, 0); }
static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{ return __okm_syscall(n, a1, a2, a3, a4, a5, a6); }

#endif
