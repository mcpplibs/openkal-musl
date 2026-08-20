/* Mapping, and the value every system call returns through.
 *
 * Two of musl's own sources are replaced here, and both for the same reason:
 * each carries a result whose width is the machine's rather than a `long's,
 * through a variable declared `long'. On every architecture musl supports the
 * two are the same width and the declaration is harmless. On one where they are
 * not, a mapping loses the upper half of its address and an offset beyond two
 * gigabytes is truncated, and neither is reported.
 *
 * They are replaced rather than edited because a replacement is visible in the
 * manifest beside its reason, and an edit is visible only to a reader who
 * thinks to look.
 */
#include "okm.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "syscall.h"

/* The value every system call returns through.
 *
 * The kernel convention musl was written for reports a failure as a small
 * negative number, and this converts it to the convention a C program expects.
 * The window is the last page of the address space, which no successful call
 * can return.
 */
syscall_arg_t __syscall_ret(syscall_uret_t r)
{
	if (r > (syscall_uret_t)-4096) {
		errno = (int)-(syscall_arg_t)r;
		return -1;
	}
	return (syscall_arg_t)r;
}

static void dummy(void) { }
weak_alias(dummy, __vm_wait);

#define UNIT SYSCALL_MMAP2_UNIT
#define OFF_MASK ((-0x2000ULL << (8*sizeof(syscall_arg_t)-1)) | (UNIT-1))

void *__mmap(void *start, size_t len, int prot, int flags, int fd, off_t off)
{
	syscall_arg_t ret;
	if (off & OFF_MASK) { errno = EINVAL; return MAP_FAILED; }
	if (len >= PTRDIFF_MAX) { errno = ENOMEM; return MAP_FAILED; }
	if (flags & MAP_FIXED) __vm_wait();

	ret = __syscall(SYS_mmap, start, len, prot, flags, fd, off);

	/* The kernel musl was written for reports a refusal to place a mapping
	 * where the caller did not ask for one as a permission failure. The
	 * caller asked for a mapping and not for a place, so the condition it
	 * met is that there was no memory. */
	if (ret == -EPERM && !start && (flags & MAP_ANON) && !(flags & MAP_FIXED))
		ret = -ENOMEM;

	return (void *)(uintptr_t)__syscall_ret((syscall_uret_t)ret);
}

weak_alias(__mmap, mmap);
