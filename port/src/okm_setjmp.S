/* setjmp, longjmp, and the names that are the same operation here.
 *
 * These are the only names in musl for which no C exists to fall back on. musl
 * ships a zero-byte source for each so that its build system does not fail when
 * the architecture's assembly is selected, so a port that drops the assembly
 * and keeps the source gets an object with nothing in it and a link that fails
 * at the last step, naming symbols nobody wrote. The measurement that found
 * them was made by reading the contents of those files rather than checking
 * that they exist.
 *
 * `sigsetjmp' is defined here as the same entry point as `setjmp', and that is
 * a statement about openkal rather than a shortcut. What sigsetjmp does beyond
 * setjmp is save and restore a signal mask; openkal has no asynchronous
 * delivery, so there is no mask, and saving nothing is what saving it amounts
 * to. musl's own arrangement --- a wrapper that calls setjmp and then
 * __sigsetjmp_tail --- exists to make a system call this port answers with
 * success and no effect, so the wrapper is the whole of what is removed.
 *
 * The instruction sequences are the calling convention's, which is why they
 * could not be written in C. The conditioning around them is for the object
 * formats and the two calling conventions this port builds for, and nothing
 * else differs between them.
 */

/* --- object format ---------------------------------------------------------- */
#if defined(__APPLE__)
#  define OKM_NAME(x) _##x
#  define OKM_DECL(x) .globl OKM_NAME(x)
#  define OKM_SIZE(x)
#elif defined(_WIN32)
#  define OKM_NAME(x) x
#  define OKM_DECL(x) .globl x; .def x; .scl 2; .type 32; .endef
#  define OKM_SIZE(x)
#elif defined(__aarch64__)
#  define OKM_NAME(x) x
#  define OKM_DECL(x) .globl x; .type x,%function
#  define OKM_SIZE(x) .size x,.-x
#else
#  define OKM_NAME(x) x
#  define OKM_DECL(x) .globl x; .type x,@function
#  define OKM_SIZE(x) .size x,.-x
#endif

	.text

/* --- x86_64, the convention this port meets on two of its three targets ----- */
#if defined(__x86_64__) && !defined(_WIN32)

OKM_DECL(__setjmp)
OKM_DECL(_setjmp)
OKM_DECL(setjmp)
OKM_DECL(sigsetjmp)
OKM_DECL(__sigsetjmp)
OKM_NAME(__setjmp):
OKM_NAME(_setjmp):
OKM_NAME(setjmp):
OKM_NAME(sigsetjmp):
OKM_NAME(__sigsetjmp):
	mov %rbx,(%rdi)
	mov %rbp,8(%rdi)
	mov %r12,16(%rdi)
	mov %r13,24(%rdi)
	mov %r14,32(%rdi)
	mov %r15,40(%rdi)
	lea 8(%rsp),%rdx        /* the stack pointer without this call's own return address */
	mov %rdx,48(%rdi)
	mov (%rsp),%rdx
	mov %rdx,56(%rdi)
	xor %eax,%eax
	ret
	OKM_SIZE(OKM_NAME(setjmp))

OKM_DECL(_longjmp)
OKM_DECL(longjmp)
OKM_NAME(_longjmp):
OKM_NAME(longjmp):
	xor %eax,%eax
	cmp $1,%esi             /* a value of zero is reported as one */
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

/* --- x86_64, the other convention -------------------------------------------
 *
 * The first argument arrives in a different register, four more registers are
 * the callee's to preserve, and ten of the vector registers are as well. The
 * last is the difference a reader is most likely to overlook: on the other
 * convention every vector register is the caller's, so musl's sequence saves
 * none, and a longjmp here that saved none would restore eight registers a
 * function between the two calls was entitled to have changed.
 */
#elif defined(__x86_64__) && defined(_WIN32)

OKM_DECL(__setjmp)
OKM_DECL(_setjmp)
OKM_DECL(setjmp)
OKM_DECL(sigsetjmp)
OKM_DECL(__sigsetjmp)
__setjmp:
_setjmp:
setjmp:
sigsetjmp:
__sigsetjmp:
	mov %rbx,(%rcx)
	mov %rbp,8(%rcx)
	mov %rdi,16(%rcx)
	mov %rsi,24(%rcx)
	mov %r12,32(%rcx)
	mov %r13,40(%rcx)
	mov %r14,48(%rcx)
	mov %r15,56(%rcx)
	lea 8(%rsp),%rax
	mov %rax,64(%rcx)
	mov (%rsp),%rax
	mov %rax,72(%rcx)
	movups %xmm6,80(%rcx)
	movups %xmm7,96(%rcx)
	movups %xmm8,112(%rcx)
	movups %xmm9,128(%rcx)
	movups %xmm10,144(%rcx)
	movups %xmm11,160(%rcx)
	movups %xmm12,176(%rcx)
	movups %xmm13,192(%rcx)
	movups %xmm14,208(%rcx)
	movups %xmm15,224(%rcx)
	xor %eax,%eax
	ret

OKM_DECL(_longjmp)
OKM_DECL(longjmp)
_longjmp:
longjmp:
	mov %edx,%eax
	test %eax,%eax
	jnz 1f
	inc %eax
1:	mov (%rcx),%rbx
	mov 8(%rcx),%rbp
	mov 16(%rcx),%rdi
	mov 24(%rcx),%rsi
	mov 32(%rcx),%r12
	mov 40(%rcx),%r13
	mov 48(%rcx),%r14
	mov 56(%rcx),%r15
	movups 80(%rcx),%xmm6
	movups 96(%rcx),%xmm7
	movups 112(%rcx),%xmm8
	movups 128(%rcx),%xmm9
	movups 144(%rcx),%xmm10
	movups 160(%rcx),%xmm11
	movups 176(%rcx),%xmm12
	movups 192(%rcx),%xmm13
	movups 208(%rcx),%xmm14
	movups 224(%rcx),%xmm15
	mov 64(%rcx),%rsp
	jmp *72(%rcx)

/* --- aarch64 ----------------------------------------------------------------- */
#elif defined(__aarch64__)

OKM_DECL(__setjmp)
OKM_DECL(_setjmp)
OKM_DECL(setjmp)
OKM_DECL(sigsetjmp)
OKM_DECL(__sigsetjmp)
OKM_NAME(__setjmp):
OKM_NAME(_setjmp):
OKM_NAME(setjmp):
OKM_NAME(sigsetjmp):
OKM_NAME(__sigsetjmp):
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

OKM_DECL(_longjmp)
OKM_DECL(longjmp)
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
	ldp  d8,  d9, [x0,#112]
	ldp d10, d11, [x0,#128]
	ldp d12, d13, [x0,#144]
	ldp d14, d15, [x0,#160]
	cmp w1, 0
	csinc w0, w1, wzr, ne
	br x30
	OKM_SIZE(OKM_NAME(longjmp))

#else
#error "openkal-musl provides setjmp for x86_64 and aarch64"
#endif

/* An executable stack is not required by anything above, and the linker assumes
 * one when an object does not say so. */
#if defined(__ELF__)
	.section .note.GNU-stack,"",%progbits
#endif
