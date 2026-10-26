/* The C library of an ok image (target `<arch>-openkal'; openkal SPEC clause 12).
 *
 * An image is entered by a loader rather than by a kernel: `okrun' maps it,
 * binds the `kal_*' names it imports to the one implementation of the process,
 * runs its initialisers and calls its entry. Everything this port already does
 * above openkal holds unchanged --- the arguments and the named values come from
 * `kal_env_*', the thread pointer is the context table of okm_context.c --- and
 * what differs is collected here:
 *
 *   ok_main            the entry SPEC 12.2 names. The loader has run the
 *                      initialisers (this library's own first, okm_early_init),
 *                      so it brings the library up if no initialiser did, runs
 *                      `main', the handlers `atexit' registered and the flush of
 *                      every stream, and returns the status; the loader then runs
 *                      the finalisers and ends the program with it.
 *   dl_iterate_phdr    the image itself, then every image `dlopen' placed.
 *   __emutls_get_address
 *                      an image's thread-local variables are emulated (SPEC
 *                      12.2); each is a key of this library's own.
 *   dlopen, dlsym, dladdr, dlclose, dlerror
 *                      libokld, the loader the image format defines, filled
 *                      from openkal.exec beneath this library. A placed image's
 *                      imports are bound to the implementation's names, to this
 *                      image's exports (one C library per process) and to the
 *                      images placed before it with RTLD_GLOBAL.
 */
#define _GNU_SOURCE   /* Dl_info */
#include "../okm.h"

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <okld.h>

void   __okm_libc_init(void);
char** __okm_argv(void);
int    __okm_argc(void);
extern char** __environ;

/* musl's own exit sequence, less the end: weak, as exit.c holds them. */
void __funcs_on_exit(void) __attribute__((__weak__));
void __stdio_exit(void) __attribute__((__weak__));

int main(int, char**, char**);

int ok_main(void)
{
	__okm_libc_init();
	const int status = main(__okm_argc(), __okm_argv(), __environ);
	if (__funcs_on_exit) __funcs_on_exit();
	if (__stdio_exit) __stdio_exit();
	return status;
}

/* --- this image ------------------------------------------------------------ */

extern const ElfW(Ehdr) __ehdr_start __attribute__((__weak__));
extern ElfW(Dyn) _DYNAMIC[] __attribute__((__weak__));

static ElfW(Addr) self_base(void)
{
	const ElfW(Ehdr) *eh = &__ehdr_start;
	if (!eh) return 0;
	const ElfW(Phdr) *ph = (const ElfW(Phdr) *)((const char *)eh + eh->e_phoff);
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD) return (ElfW(Addr))eh - ph[i].p_vaddr;
	return (ElfW(Addr))eh;
}

/* This image's own export of `name', through its SysV hash table (the image is
 * linked with both tables, SPEC 12.2). */
static void *self_symbol(const char *name)
{
	if (!_DYNAMIC) return 0;
	const ElfW(Addr) base = self_base();
	const ElfW(Sym) *symtab = 0;
	const char *strtab = 0;
	const ElfW(Word) *hash = 0;
	for (const ElfW(Dyn) *d = _DYNAMIC; d->d_tag != DT_NULL; d++) {
		if (d->d_tag == DT_SYMTAB) symtab = (const ElfW(Sym) *)(base + d->d_un.d_ptr);
		else if (d->d_tag == DT_STRTAB) strtab = (const char *)(base + d->d_un.d_ptr);
		else if (d->d_tag == DT_HASH) hash = (const ElfW(Word) *)(base + d->d_un.d_ptr);
	}
	if (!symtab || !strtab || !hash) return 0;
	unsigned long h = 0;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		h = (h << 4) + *p;
		const unsigned long g = h & 0xf0000000ul;
		if (g) h ^= g >> 24;
		h &= ~g;
	}
	const ElfW(Word) nbucket = hash[0];
	const ElfW(Word) *bucket = hash + 2, *chain = bucket + nbucket;
	for (ElfW(Word) i = bucket[h % nbucket]; i; i = chain[i]) {
		const ElfW(Sym) *s = symtab + i;
		if (s->st_shndx == SHN_UNDEF || ELF64_ST_BIND(s->st_info) == STB_LOCAL) continue;
		if (strcmp(strtab + s->st_name, name) == 0) return (void *)(base + s->st_value);
	}
	return 0;
}

/* --- placed images --------------------------------------------------------- */

struct placed {
	struct okld_module *m;
	struct placed *next;
	int refs;
	int global;
};
static struct placed *g_placed;
static pthread_mutex_t g_dl_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_dl_error[512];
static int g_dl_error_set;

static void dl_fail(const char *a, const char *b)
{
	snprintf(g_dl_error, sizeof g_dl_error, "%s%s", a, b ? b : "");
	g_dl_error_set = 1;
}

char *dlerror(void)
{
	if (!g_dl_error_set) return 0;
	g_dl_error_set = 0;
	return g_dl_error;
}

void *__okm_kal_resolve(const char *name);   /* okm_kal_table.c */

static void *host_reserve(okld_size n, void *ctx) { (void)ctx; return kal_exec_alloc(n); }
static int host_publish(void *r, okld_size off, okld_size n, void *ctx) { (void)ctx; return kal_exec_publish_part(r, off, n); }
static void host_release(void *r, okld_size n, void *ctx) { (void)ctx; kal_exec_free(r, n); }
static okld_size host_granularity(void *ctx) { (void)ctx; return kal_exec_granularity(); }
static void *host_alloc(okld_size n, void *ctx) { (void)ctx; return malloc(n); }
static void host_free(void *p, okld_size n, void *ctx) { (void)ctx; (void)n; free(p); }

static void *host_resolve(const char *name, void *ctx)
{
	(void)ctx;
	void *s = 0;
	if (strncmp(name, "kal_", 4) == 0 && (s = __okm_kal_resolve(name))) return s;
	if ((s = self_symbol(name))) return s;
	for (struct placed *p = g_placed; p; p = p->next)
		if (p->global && (s = okld_symbol(p->m, name))) return s;
	return 0;
}

static void *read_whole(const char *path, okld_size *size)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	size_t cap = 1 << 16, n = 0;
	unsigned char *buf = malloc(cap);
	while (buf) {
		const size_t got = fread(buf + n, 1, cap - n, f);
		n += got;
		if (n < cap) break;
		unsigned char *more = realloc(buf, cap * 2);
		if (!more) { free(buf); buf = 0; break; }
		buf = more;
		cap *= 2;
	}
	fclose(f);
	*size = n;
	return buf;
}

/* An image another names (DT_NEEDED) is looked for beside the image that named
 * it, then beside this one. */
static char g_dl_dir[1024];

static const void *host_find_needed(const char *name, okld_size *size, void *ctx)
{
	(void)ctx;
	char path[1536];
	void *b;
	snprintf(path, sizeof path, "%s/%s", g_dl_dir, name);
	if ((b = read_whole(path, size))) return b;
	const kal_intptr n = kal_env_image(path, sizeof path - 1);
	if (n > 0) {
		path[n] = 0;
		char *slash = strrchr(path, '/');
		if (slash) {
			snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "%s", name);
			if ((b = read_whole(path, size))) return b;
		}
	}
	return 0;
}

static void host_release_bytes(const void *b, okld_size n, void *ctx) { (void)n; (void)ctx; free((void *)b); }

static void call_image(void *fn, void *ctx) { (void)ctx; ((void (*)(void))fn)(); }

void *dlopen(const char *path, int flags)
{
	if (!path) return RTLD_DEFAULT;
	pthread_mutex_lock(&g_dl_lock);
	for (struct placed *p = g_placed; p; p = p->next)
		if (strcmp(okld_name(p->m), path) == 0) {
			p->refs++;
			if (flags & RTLD_GLOBAL) p->global = 1;
			pthread_mutex_unlock(&g_dl_lock);
			return p;
		}
	okld_size size = 0;
	void *bytes = read_whole(path, &size);
	if (!bytes) {
		dl_fail("cannot read ", path);
		pthread_mutex_unlock(&g_dl_lock);
		return 0;
	}
	snprintf(g_dl_dir, sizeof g_dl_dir, "%s", path);
	char *slash = strrchr(g_dl_dir, '/');
	if (slash) *slash = 0; else strcpy(g_dl_dir, ".");
	/* Static: the loader keeps the pointer for the module's life and calls
	 * through it again when the module is unloaded. */
	static const struct okld_host host = {
		host_reserve, host_publish, host_release, host_granularity, host_resolve,
		host_find_needed, host_release_bytes, host_alloc, host_free, 0, 0,
	};
	struct okld_module *m = 0;
	struct okld_why why;
	const int rc = okld_load(bytes, size, path, &host, &m, &why);
	free(bytes);
	if (rc != 0) {
		dl_fail(path, ": ");
		strncat(g_dl_error, why.text, sizeof g_dl_error - strlen(g_dl_error) - 1);
		pthread_mutex_unlock(&g_dl_lock);
		return 0;
	}
	struct placed *p = malloc(sizeof *p);
	p->m = m;
	p->refs = 1;
	p->global = (flags & RTLD_GLOBAL) != 0;
	p->next = g_placed;
	g_placed = p;
	pthread_mutex_unlock(&g_dl_lock);
	okld_initialise(m, call_image, 0);
	return p;
}

void *dlsym(void *restrict handle, const char *restrict name)
{
	void *s = 0;
	pthread_mutex_lock(&g_dl_lock);
	if (handle == RTLD_DEFAULT) {
		s = host_resolve(name, 0);
	} else {
		for (struct placed *p = g_placed; p; p = p->next)
			if (p == handle) { s = okld_symbol(p->m, name); break; }
	}
	if (!s) dl_fail("symbol not found: ", name);
	pthread_mutex_unlock(&g_dl_lock);
	return s;
}

int dlclose(void *handle)
{
	pthread_mutex_lock(&g_dl_lock);
	struct placed **at = &g_placed;
	while (*at && *at != handle) at = &(*at)->next;
	if (!*at) {
		dl_fail("dlclose: not an open image", 0);
		pthread_mutex_unlock(&g_dl_lock);
		return -1;
	}
	struct placed *p = *at;
	if (--p->refs > 0) { pthread_mutex_unlock(&g_dl_lock); return 0; }
	*at = p->next;
	pthread_mutex_unlock(&g_dl_lock);
	okld_finalise(p->m, call_image, 0);
	okld_unload(p->m);
	free(p);
	return 0;
}

int dladdr(const void *addr, Dl_info *info)
{
	struct okld_module *m = okld_containing(addr);
	if (m) {
		info->dli_fname = okld_name(m);
		info->dli_fbase = okld_base(m);
		info->dli_sname = 0;
		info->dli_saddr = 0;
		return 1;
	}
	/* This image: the symbol whose range holds the address. */
	if (!_DYNAMIC) return 0;
	const ElfW(Addr) base = self_base();
	const ElfW(Sym) *symtab = 0;
	const char *strtab = 0;
	const ElfW(Word) *hash = 0;
	for (const ElfW(Dyn) *d = _DYNAMIC; d->d_tag != DT_NULL; d++) {
		if (d->d_tag == DT_SYMTAB) symtab = (const ElfW(Sym) *)(base + d->d_un.d_ptr);
		else if (d->d_tag == DT_STRTAB) strtab = (const char *)(base + d->d_un.d_ptr);
		else if (d->d_tag == DT_HASH) hash = (const ElfW(Word) *)(base + d->d_un.d_ptr);
	}
	if (!symtab || !strtab || !hash) return 0;
	const ElfW(Addr) a = (ElfW(Addr))addr;
	const ElfW(Sym) *best = 0;
	for (ElfW(Word) i = 1; i < hash[1]; i++) {
		const ElfW(Sym) *s = symtab + i;
		if (s->st_shndx == SHN_UNDEF || !s->st_value) continue;
		const ElfW(Addr) at = base + s->st_value;
		if (at <= a && (!best || at > base + best->st_value)
		    && (s->st_size == 0 || a < at + s->st_size))
			best = s;
	}
	static char self_name[1024];
	const kal_intptr n = kal_env_image(self_name, sizeof self_name - 1);
	self_name[n > 0 ? n : 0] = 0;
	info->dli_fname = self_name;
	info->dli_fbase = (void *)base;
	info->dli_sname = best ? strtab + best->st_name : 0;
	info->dli_saddr = best ? (void *)(base + best->st_value) : 0;
	return 1;
}

/* --- dl_iterate_phdr ------------------------------------------------------- */

struct iterate { int (*callback)(struct dl_phdr_info *, size_t, void *); void *data; int result; };

static int each_placed(void *base, const void *phdr, okld_u32 phnum, const char *name, void *ctx)
{
	struct iterate *it = ctx;
	struct dl_phdr_info info;
	memset(&info, 0, sizeof info);
	info.dlpi_addr  = (ElfW(Addr))base;
	info.dlpi_name  = name;
	info.dlpi_phdr  = phdr;
	info.dlpi_phnum = (ElfW(Half))phnum;
	info.dlpi_adds  = 1;
	it->result = it->callback(&info, sizeof info, it->data);
	return it->result;
}

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size, void *data), void *data)
{
	const ElfW(Ehdr) *eh = &__ehdr_start;
	if (eh) {
		struct dl_phdr_info info;
		memset(&info, 0, sizeof info);
		info.dlpi_addr  = self_base();
		info.dlpi_name  = "";
		info.dlpi_phdr  = (const ElfW(Phdr) *)((const char *)eh + eh->e_phoff);
		info.dlpi_phnum = eh->e_phnum;
		info.dlpi_adds  = 1;
		const int r = callback(&info, sizeof info, data);
		if (r) return r;
	}
	struct iterate it = { callback, data, 0 };
	okld_iterate(each_placed, &it);
	return it.result;
}

/* --- emulated thread-local storage (SPEC 12.2) ----------------------------- */

struct __emutls_control {
	size_t size;
	size_t align;
	union { uintptr_t index; void *address; } object;
	void *value;
};

static pthread_mutex_t g_emutls_lock = PTHREAD_MUTEX_INITIALIZER;

void *__emutls_get_address(struct __emutls_control *c)
{
	uintptr_t key = __atomic_load_n(&c->object.index, __ATOMIC_ACQUIRE);
	if (!key) {
		pthread_mutex_lock(&g_emutls_lock);
		if (!c->object.index) {
			pthread_key_t k;
			if (pthread_key_create(&k, free) != 0) abort();
			__atomic_store_n(&c->object.index, (uintptr_t)k + 1, __ATOMIC_RELEASE);
		}
		key = c->object.index;
		pthread_mutex_unlock(&g_emutls_lock);
	}
	void *p = pthread_getspecific((pthread_key_t)(key - 1));
	if (!p) {
		const size_t align = c->align < sizeof(void *) ? sizeof(void *) : c->align;
		p = aligned_alloc(align, (c->size + align - 1) / align * align);
		if (!p) abort();
		if (c->value) memcpy(p, c->value, c->size);
		else memset(p, 0, c->size);
		pthread_setspecific((pthread_key_t)(key - 1), p);
	}
	return p;
}
