/* Operations upon an open file description.
 *
 * ⚠️⚠️ THE ELEVENTH SOURCE THIS PORT REPLACES, AND IT IS THE THIRD OF ONE KIND:
 * A MACHINE WORD CARRIED THROUGH A VARIABLE DECLARED `long'.
 *
 * musl's own reads its variable argument as an `unsigned long':
 *
 *     unsigned long arg;
 *     arg = va_arg(ap, unsigned long);
 *     ...
 *     case F_SETLK: return syscall(SYS_fcntl, fd, cmd, (void *)arg);
 *
 * That is correct on every system musl was written for, where a `long' holds a
 * pointer. It is not correct on one this port builds for, where it holds
 * thirty-two bits and a pointer holds sixty-four --- so a caller passing a
 * `struct flock *' had the top half of it discarded before the port saw it.
 *
 * ⭐⭐ AND IT WAS UNREACHABLE UNTIL 0.11.0, WHICH IS WHY IT SURVIVED THREE
 * RELEASES OF A PORT THAT ALREADY NAMED THIS DEFECT TWICE.
 *
 * Every command this library answered before took an integer, or took a pointer
 * it never followed: `F_SETLK' returned 0 and did nothing, and later `ENOSYS'.
 * A truncated pointer that nothing dereferences is a truncated pointer nothing
 * reports. openkal 0.10 gave this port a real lock, `F_SETLK' began following
 * the pointer, and the defect became a fault on the first attempt.
 *
 * ⚠️ MEASURED, and the register file names the type rather than the symptom:
 *
 *     page fault on read access to 0x00000000fe2ffec2
 *     rax:00000000fe2ffec0   rsp:00007ffffe2fc7a0
 *     winlk+0x1e67f: movzxw 0x02(%rax), %eax
 *
 * `rax' is the caller's `struct flock *' with its top thirty-two bits gone, and
 * the offset it faults at --- two --- is `l_whence', the first field this port
 * reads. The pointer was already ruined when the port received it.
 *
 * ⇒ Replaced rather than patched, which is what this port does with the other
 * two of this kind (`mmap' and `getcwd'): the value never becomes a narrower
 * integer at all. `uintptr_t' is the type that holds a pointer on every target,
 * and it is what `va_arg' is asked for.
 *
 * ⚠️ THE VARARG TYPE IS PART OF THE CALLING CONVENTION AND NOT A DETAIL.
 * `va_arg(ap, unsigned long)' and `va_arg(ap, uintptr_t)' read different numbers
 * of bytes where the two types differ, so this is not a cast applied afterwards
 * --- reading it as the narrower type has already lost the half by then.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include "syscall.h"

int fcntl(int fd, int cmd, ...)
{
	/* The one changed line, and the whole of the change. */
	uintptr_t arg;
	va_list ap;
	va_start(ap, cmd);
	arg = va_arg(ap, uintptr_t);
	va_end(ap);
	if (cmd == F_SETFL) arg |= O_LARGEFILE;
	if (cmd == F_SETLKW) return syscall_cp(SYS_fcntl, fd, cmd, (void *)arg);
	if (cmd == F_GETOWN) {
		struct f_owner_ex ex;
		int ret = __syscall(SYS_fcntl, fd, F_GETOWN_EX, &ex);
		if (ret == -EINVAL) return __syscall(SYS_fcntl, fd, cmd, (void *)arg);
		if (ret) return __syscall_ret(ret);
		return ex.type == F_OWNER_PGRP ? -ex.pid : ex.pid;
	}
	if (cmd == F_DUPFD_CLOEXEC) {
		int ret = __syscall(SYS_fcntl, fd, F_DUPFD_CLOEXEC, arg);
		if (ret != -EINVAL) {
			if (ret >= 0)
				__syscall(SYS_fcntl, ret, F_SETFD, FD_CLOEXEC);
			return __syscall_ret(ret);
		}
		ret = __syscall(SYS_fcntl, fd, F_DUPFD_CLOEXEC, 0);
		if (ret != -EINVAL) {
			if (ret >= 0) __syscall(SYS_close, ret);
			return __syscall_ret(-EINVAL);
		}
		ret = __syscall(SYS_fcntl, fd, F_DUPFD, arg);
		if (ret >= 0) __syscall(SYS_fcntl, ret, F_SETFD, FD_CLOEXEC);
		return __syscall_ret(ret);
	}
	switch (cmd) {
	case F_SETLK:
	case F_GETLK:
	case F_GETOWN_EX:
	case F_SETOWN_EX:
		return syscall(SYS_fcntl, fd, cmd, (void *)arg);
	default:
		return syscall(SYS_fcntl, fd, cmd, arg);
	}
}
