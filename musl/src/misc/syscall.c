#define _BSD_SOURCE
#include <unistd.h>
#include "syscall.h"
#include <stdarg.h>
#include <stdint.h>

#undef syscall

long syscall(long n, ...)
{
	va_list ap;
	syscall_arg_t a,b,c,d,e,f;
	/* openkal-musl: each argument is read as the pointer-wide word its caller
	 * passed (a `long' or a pointer), then widened to this port's argument
	 * word, which is sixty-four bits where `long' is narrower (i386, and the
	 * LLP64 model). Read as the wider word, two arguments of i386 were one
	 * (PATCHES.md, "i386 (0.29.5)"). */
	va_start(ap, n);
	a=(syscall_arg_t)va_arg(ap, intptr_t);
	b=(syscall_arg_t)va_arg(ap, intptr_t);
	c=(syscall_arg_t)va_arg(ap, intptr_t);
	d=(syscall_arg_t)va_arg(ap, intptr_t);
	e=(syscall_arg_t)va_arg(ap, intptr_t);
	f=(syscall_arg_t)va_arg(ap, intptr_t);
	va_end(ap);
	return __syscall_ret(__syscall(n,a,b,c,d,e,f));
}
