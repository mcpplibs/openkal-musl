/* setjmp, longjmp and sigsetjmp.
 *
 * These are the only four names in musl for which no C exists to fall back on.
 * musl ships a zero-byte source for each so that its build system does not
 * fail when the architecture's assembly is selected; a port that drops the
 * assembly and keeps the source gets an object with nothing in it and a link
 * that fails at the last step, naming symbols nobody wrote. The measurement
 * that found this was made by inspecting the contents of the files rather than
 * their existence, which is the only form of the measurement that is true.
 *
 * The sequences are musl's own, unchanged. What is added is the small amount
 * of conditioning that lets one file assemble for more than one object format:
 * the leading underscore some formats put on a C name, and the directives that
 * exist in one format and not another. The instructions themselves are
 * identical, because the calling convention is the processor's rather than the
 * environment's --- which is the reason the four names could not be written in
 * C in the first place.
 */

#if defined(__APPLE__)
#  define OKM_NAME(x) _##x
#  define OKM_TYPE(x)
#  define OKM_SIZE(x)
#  define OKM_HIDDEN(x) .private_extern OKM_NAME(x)
#  define OKM_CALL(x) OKM_NAME(x)
#elif defined(_WIN32)
#  define OKM_NAME(x) x
#  define OKM_TYPE(x) .def x; .scl 2; .type 32; .endef
#  define OKM_SIZE(x)
#  define OKM_HIDDEN(x)
#  define OKM_CALL(x) x
#else
#  define OKM_NAME(x) x
#  if defined(__aarch64__)
#    define OKM_TYPE(x) .type x,%function
#  else
#    define OKM_TYPE(x) .type x,@function
#  endif
#  define OKM_SIZE(x) .size x,.-x
#  define OKM_HIDDEN(x) .hidden x
#  define OKM_CALL(x) x
#endif

#define OKM_GLOBAL(x) .globl OKM_NAME(x); OKM_TYPE(OKM_NAME(x))

	.text

/* ------------------------------------------------------------------ x86_64 */
#if defined(__x86_64__)

OKM_GLOBAL(__setjmp)
OKM_GLOBAL(_setjmp)
OKM_GLOBAL(setjmp)
OKM_NAME(__setjmp):
OKM_NAME(_setjmp):
OKM_NAME(setjmp):
	mov %rbx,(%rdi)
	mov %rbp,8(%rdi)
	mov %r12,16(%rdi)
	mov %r13,24(%rdi)
	mov %r14,32(%rdi)
	mov %r15,40(%rdi)
	lea 8(%rsp),%rdx
	mov %rdx,48(%rdi)
	mov (%rsp),%rdx
	mov %rdx,56(%rdi)
	xor %eax,%eax
	ret
	OKM_SIZE(OKM_NAME(setjmp))

OKM_GLOBAL(_longjmp)
OKM_GLOBAL(longjmp)
OKM_NAME(_longjmp):
OKM_NAME(longjmp):
	xor %eax,%eax
	cmp $1,%esi
	adc %esi,%eax
	mov (%rdi),%rbx
	mov 8(%rdi),%rbp
	mov 16(%rdi),%r12
	mov 24(%rdi),%r13
	mov 32(%rdi),%r14
	mov 40(%rdi),%r15
	mov 48(%rdi),%rsp
	jmp *56(%rdi)
	OKM_SIZE(OKM_NAME(longjmp))

OKM_GLOBAL(sigsetjmp)
OKM_GLOBAL(__sigsetjmp)
OKM_NAME(sigsetjmp):
OKM_NAME(__sigsetjmp):
	test %esi,%esi
	jz 1f
	popq 64(%rdi)
	mov %rbx,72+8(%rdi)
	mov %rdi,%rbx
	call OKM_CALL(setjmp)
	pushq 64(%rbx)
	mov %rbx,%rdi
	mov %eax,%esi
	mov 72+8(%rbx),%rbx
	jmp OKM_CALL(__sigsetjmp_tail)
1:	jmp OKM_CALL(setjmp)
	OKM_SIZE(OKM_NAME(sigsetjmp))

/* ----------------------------------------------------------------- aarch64 */
#elif defined(__aarch64__)

OKM_GLOBAL(__setjmp)
OKM_GLOBAL(_setjmp)
OKM_GLOBAL(setjmp)
OKM_NAME(__setjmp):
OKM_NAME(_setjmp):
OKM_NAME(setjmp):
	stp x19, x20, [x0,#0]
	stp x21, x22, [x0,#16]
	stp x23, x24, [x0,#32]
	stp x25, x26, [x0,#48]
	stp x27, x28, [x0,#64]
	stp x29, x30, [x0,#80]
	mov x2, sp
	str x2, [x0,#104]
	stp  d8,  d9, [x0,#112]
	stp d10, d11, [x0,#128]
	stp d12, d13, [x0,#144]
	stp d14, d15, [x0,#160]
	mov x0, #0
	ret
	OKM_SIZE(OKM_NAME(setjmp))

OKM_GLOBAL(_longjmp)
OKM_GLOBAL(longjmp)
OKM_NAME(_longjmp):
OKM_NAME(longjmp):
	ldp x19, x20, [x0,#0]
	ldp x21, x22, [x0,#16]
	ldp x23, x24, [x0,#32]
	ldp x25, x26, [x0,#48]
	ldp x27, x28, [x0,#64]
	ldp x29, x30, [x0,#80]
	ldr x2, [x0,#104]
	mov sp, x2
	ldp d8 , d9, [x0,#112]
	ldp d10, d11, [x0,#128]
	ldp d12, d13, [x0,#144]
	ldp d14, d15, [x0,#160]
	cmp w1, 0
	csinc w0, w1, wzr, ne
	br x30
	OKM_SIZE(OKM_NAME(longjmp))

OKM_GLOBAL(sigsetjmp)
OKM_GLOBAL(__sigsetjmp)
OKM_NAME(sigsetjmp):
OKM_NAME(__sigsetjmp):
	cbz x1, OKM_CALL(setjmp)
	str x30,[x0,#176]
	str x19,[x0,#176+8+8]
	mov x19,x0
	bl OKM_CALL(setjmp)
	mov w1,w0
	mov x0,x19
	ldr x30,[x0,#176]
	ldr x19,[x0,#176+8+8]
	b OKM_CALL(__sigsetjmp_tail)
	OKM_SIZE(OKM_NAME(sigsetjmp))

#else
#error "openkal-musl provides setjmp for x86_64 and aarch64"
#endif
