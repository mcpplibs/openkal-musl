/* Startup, and the thread pointer.
 *
 * Two of musl's own sources are replaced rather than redirected, and both for
 * the same reason: they are the two places where musl reads the shape of the
 * environment directly instead of asking the kernel for something.
 *
 *   src/env/__libc_start_main.c reads the auxiliary vector, which is a table
 *   the Linux kernel leaves above the environment strings on the initial
 *   stack. No other environment has one, and openkal supplies what the vector
 *   carried --- the arguments, the named values --- through openkal.env.
 *
 *   src/env/__init_tls.c reads the program's own ELF headers in order to build
 *   the thread-local storage image. openkal reports, through kal_task_props,
 *   that a started context already observes the toolchain's thread-local
 *   storage, so there is no image for this layer to build: musl needs one slot
 *   in it, and that slot is __okm_tp.
 *
 * Everything else musl does at startup is unchanged and runs from here.
 */
#include "okm.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "libc.h"
#include "pthread_impl.h"

/* The slot musl's per-context state is reached through. Clause 7.10. */
__thread uintptr_t __okm_tp;

volatile int __thread_list_lock;

/* musl installs a thread pointer through this on Linux. Here the pointer is a
 * variable rather than a register, so installing it is an assignment; the
 * function reports success, and reporting success is what tells musl that this
 * environment can support more than one execution context. */
int __set_thread_area(void* p)
{
	__okm_tp = (uintptr_t)p;
	return 0;
}

/* The storage the first context's descriptor occupies. musl allocates every
 * later one on the stack it maps for the context. */
static struct {
	uintptr_t dtv[2];
	struct pthread pt;
	void* space[8];
} builtin_tls[1];

void* __copy_tls(unsigned char* mem)
{
	/* The program's own thread-local variables are the toolchain's business,
	 * not this layer's: openkal requires that a started context observe them,
	 * so there is no image to copy. What remains is musl's own descriptor and
	 * the vector its accessors expect to find in front of it. */
	uintptr_t* dtv = (uintptr_t*)mem;
	dtv[0] = 0;
	dtv[1] = 0;
	mem += 2 * sizeof(uintptr_t);
	mem += -(uintptr_t)mem & (_Alignof(struct pthread) - 1);
	struct pthread* td = (struct pthread*)mem;
	memset(td, 0, sizeof *td);
	td->dtv = dtv;
	return td;
}

int __init_tp(void* p)
{
	pthread_t td = p;
	td->self = td;
	if (__set_thread_area(TP_ADJ(p)) < 0) return -1;
	libc.can_do_threads = 1;
	td->detach_state = DT_JOINABLE;
	td->tid = (int)kal_task_current();
	if (td->tid == 0) td->tid = 1;
	td->locale = &libc.global_locale;
	td->robust_list.head = &td->robust_list.head;
	td->next = td->prev = td;
	return 0;
}

void __init_tls(size_t* aux)
{
	(void)aux;
	libc.tls_cnt = 0;
	libc.tls_head = 0;
	libc.tls_align = _Alignof(struct pthread) < 16 ? 16 : _Alignof(struct pthread);
	libc.tls_size = 2 * sizeof(uintptr_t) + sizeof(struct pthread) + libc.tls_align;
	if (__init_tp(__copy_tls((unsigned char*)builtin_tls)) < 0) {
		static const char m[] = "openkal-musl: the environment supplies no per-context storage\n";
		kal_abort(m, sizeof m - 1);
	}
}

/* --- the environment --------------------------------------------------------- */

extern char** __environ;

static char*  g_argv_store[256];
static char*  g_envp_store[512];
static int    g_argc;

static char* dup_counted(const char* s, size_t n)
{
	char* p = kal_alloc(n + 1, 1);
	if (!p) return 0;
	for (size_t i = 0; i < n; i++) p[i] = s[i];
	p[n] = 0;
	return p;
}

/* The arguments and the named values are taken from openkal rather than from
 * the stack the program started on. It is the same information; the difference
 * is that every environment supplies it this way and only one supplies it the
 * other way. */
void __okm_init_env(void)
{
	const kal_uintptr argc = kal_env_arg_count();
	g_argc = 0;
	for (kal_uintptr i = 0; i < argc && g_argc < 255; i++) {
		kal_uintptr len = 0;
		const char* a = kal_env_arg(i, &len);
		if (!a) break;
		char* p = dup_counted(a, len);
		if (!p) break;
		g_argv_store[g_argc++] = p;
	}
	g_argv_store[g_argc] = 0;

	int envc = 0;
	const kal_uintptr n = kal_env_var_count();
	for (kal_uintptr i = 0; i < n && envc < 511; i++) {
		kal_uintptr nlen = 0, vlen = 0;
		const char* value = 0;
		const char* name = kal_env_var_at(i, &nlen, &value, &vlen);
		if (!name) break;
		char* p = kal_alloc(nlen + vlen + 2, 1);
		if (!p) break;
		for (kal_uintptr k = 0; k < nlen; k++) p[k] = name[k];
		p[nlen] = '=';
		for (kal_uintptr k = 0; k < vlen; k++) p[nlen + 1 + k] = value[k];
		p[nlen + 1 + vlen] = 0;
		g_envp_store[envc++] = p;
	}
	g_envp_store[envc] = 0;
	__environ = g_envp_store;

	__progname = __progname_full = g_argc ? g_argv_store[0] : (char*)"";
	for (char* q = __progname_full; *q; q++) if (*q == '/') __progname = q + 1;
}

char** __okm_argv(void) { return g_argv_store; }
int    __okm_argc(void) { return g_argc; }

/* --- the hand-over ----------------------------------------------------------- */

static void dummy(void) { }
weak_alias(dummy, _init);
weak_alias(dummy, _fini);

extern weak hidden void (*const __init_array_start)(void), (*const __init_array_end)(void);

static void libc_start_init(void)
{
	_init();
	uintptr_t a = (uintptr_t)&__init_array_start;
	for (; a < (uintptr_t)&__init_array_end; a += sizeof(void (*)()))
		(*(void (**)(void))a)();
}

weak_alias(libc_start_init, __libc_start_init);

/* The name every C library uses for the hand-over from the object that
 * receives control to the library itself. The openkal implementation supplies
 * that object, because what it does --- finding the arguments, establishing
 * the thread pointer --- is a fact about the environment rather than about the
 * library.
 *
 * The arguments this function is given are ignored, and that is the point: an
 * environment whose entry point has no argument vector to pass calls it with
 * none, and the vector is obtained here from openkal.env either way. */
/* The auxiliary vector, which the Linux kernel leaves above the environment
 * strings and which no other environment has. musl reads three entries from
 * it, so three entries are supplied rather than the pointer being left null:
 * the allocator dereferences it without checking, which is correct of it,
 * because on the environment musl was written for the vector always exists.
 *
 * AT_RANDOM is the interesting one. openkal has no source of entropy and this
 * layer does not invent one: the bytes below are derived from the clock and
 * from the address of an object, which makes them unpredictable to a reader of
 * the source and not to an adversary. They are used for the allocator's
 * internal cookie and for the stack canary, and the README records that
 * neither is a security property on this port. */
static size_t        g_auxv[7];
static unsigned char g_random[16];

static void fill_random(void)
{
	__UINT64_TYPE__ x = kal_time_wall() ^ (kal_time_monotonic() * 6364136223846793005u);
	x ^= (__UINT64_TYPE__)(uintptr_t)&g_random;
	if (!x) x = 88172645463325252u;
	for (int i = 0; i < 16; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		g_random[i] = (unsigned char)(x >> 24);
	}
}

int __libc_start_main(int (*main_fn)(int, char**, char**), int argc, char** argv,
                      void (*init_dummy)(), void (*fini_dummy)(), void (*ldso_dummy)())
{
	(void)argc; (void)argv; (void)init_dummy; (void)fini_dummy; (void)ldso_dummy;

	libc.page_size = 4096;
	fill_random();
	g_auxv[0] = 6  /* AT_PAGESZ */; g_auxv[1] = 4096;
	g_auxv[2] = 25 /* AT_RANDOM */; g_auxv[3] = (size_t)(uintptr_t)g_random;
	g_auxv[4] = 0;
	libc.auxv = g_auxv;

	__okm_init_env();
	__init_tls(0);
	__init_ssp(g_random);
	okm_table_init();

	__libc_start_init();
	exit(main_fn(g_argc, g_argv_store, __environ));
	return 0;
}
