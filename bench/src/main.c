/* The primitives a program built on this library spends its time in, timed
 * (0.26.0; the layer the design calls L1).
 *
 * ONE SOURCE, BUILT THREE WAYS. As a program of the build machine's own C
 * library (`cc -O2 bench/src/main.c'), as a program above this library on the
 * machine's openkal implementation (`mcpp build --release' here), and as an ok
 * image run by okrun (`--target <arch>-openkal'). tools/bench-compare.sh reads
 * two of its reports and divides: the ratio of this library to the system's,
 * per line, is what the gate is on.
 *
 * WHAT A LINE IS. `bench <name> <value> <unit>', the value the median of five
 * runs of about 40 ms each after one unmeasured run. `ns' is per operation;
 * `faults' is minor page faults per gigabyte allocated and released, read from
 * the system's record of this process where there is one (Linux, /proc).
 *
 *   bench [--quick]       every measurement (--quick: shorter runs, for a
 *                         check that the program works rather than a timing) */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double g_run_ns = 40e6;

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* Keeps a value the compiler would otherwise prove unused. */
static volatile uintptr_t g_sink;
#define KEEP(x) (g_sink += (uintptr_t)(x))

typedef void (*body_fn)(void* arg, long iterations);

static int cmp_double(const void* a, const void* b)
{
	const double x = *(const double*)a, y = *(const double*)b;
	return (x > y) - (x < y);
}

/* Nanoseconds per iteration of `body', the median of five runs. */
static double measure(body_fn body, void* arg)
{
	/* One unmeasured run first: a first thread, a first mapping and a cold
	 * cache are not what the line reports. */
	body(arg, 1);
	long n = 1;
	for (;;) {   /* an iteration count that takes about one run's time */
		const uint64_t t0 = now_ns();
		body(arg, n);
		const uint64_t dt = now_ns() - t0;
		if (dt > g_run_ns / 4 || n > (1L << 40)) { n = (long)((double)n * g_run_ns / (double)(dt ? dt : 1)) + 1; break; }
		n *= 4;
	}
	double r[5];
	for (int i = 0; i < 5; i++) {
		const uint64_t t0 = now_ns();
		body(arg, n);
		r[i] = (double)(now_ns() - t0) / (double)n;
	}
	qsort(r, 5, sizeof r[0], cmp_double);
	return r[2];
}

static void report(const char* name, double value, const char* unit)
{
	printf("bench %s %.2f %s\n", name, value, unit);
	fflush(stdout);
}

/* --- strings ----------------------------------------------------------------- */

static unsigned char g_a[1 << 17], g_b[1 << 17];

struct sarg { size_t n, align; };

static void b_strlen(void* p, long it)
{
	const struct sarg* a = p;
	char* s = (char*)g_a + a->align;
	memset(s, 'x', a->n); s[a->n] = 0;
	for (long i = 0; i < it; i++) { KEEP(strlen(s)); __asm__ __volatile__("" ::: "memory"); }
}
static void b_memcpy(void* p, long it)
{
	const struct sarg* a = p;
	for (long i = 0; i < it; i++) { memcpy(g_b + a->align, g_a, a->n); __asm__ __volatile__("" ::: "memory"); }
}
static void b_memset(void* p, long it)
{
	const struct sarg* a = p;
	for (long i = 0; i < it; i++) { memset(g_b + a->align, (int)i, a->n); __asm__ __volatile__("" ::: "memory"); }
}
static void b_memcmp(void* p, long it)
{
	const struct sarg* a = p;
	memcpy(g_b + a->align, g_a, a->n);
	for (long i = 0; i < it; i++) { KEEP(memcmp(g_b + a->align, g_a, a->n)); __asm__ __volatile__("" ::: "memory"); }
}
static void b_memchr(void* p, long it)
{
	const struct sarg* a = p;
	memset(g_a, 'x', a->n + 64); g_a[a->n - 1] = 'y';
	for (long i = 0; i < it; i++) { KEEP(memchr(g_a, 'y', a->n)); __asm__ __volatile__("" ::: "memory"); }
}
static void b_strcmp(void* p, long it)
{
	const struct sarg* a = p;
	char* l = (char*)g_a + a->align;
	char* r = (char*)g_b + 3;
	memset(l, 'q', a->n); l[a->n] = 0;
	memcpy(r, l, a->n + 1);
	for (long i = 0; i < it; i++) { KEEP(strcmp(l, r)); __asm__ __volatile__("" ::: "memory"); }
}

static void strings(void)
{
	static const size_t lens[] = { 8, 16, 32, 64, 256, 4096 };
	char name[64];
	for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++)
		for (size_t al = 0; al < 8; al += 7) {
			struct sarg a = { lens[i], al };
			snprintf(name, sizeof name, "strlen/%zu/%zu", a.n, al); report(name, measure(b_strlen, &a), "ns");
			snprintf(name, sizeof name, "memcpy/%zu/%zu", a.n, al); report(name, measure(b_memcpy, &a), "ns");
			snprintf(name, sizeof name, "memcmp/%zu/%zu", a.n, al); report(name, measure(b_memcmp, &a), "ns");
			snprintf(name, sizeof name, "strcmp/%zu/%zu", a.n, al); report(name, measure(b_strcmp, &a), "ns");
		}
	static const size_t big[] = { 64, 4096, 65536 };
	for (size_t i = 0; i < 3; i++) {
		struct sarg a = { big[i], 0 };
		snprintf(name, sizeof name, "memset/%zu", a.n); report(name, measure(b_memset, &a), "ns");
		snprintf(name, sizeof name, "memchr/%zu", a.n); report(name, measure(b_memchr, &a), "ns");
	}
	struct sarg a = { 65536, 0 };
	report("memcpy/65536/0", measure(b_memcpy, &a), "ns");
}

/* --- allocation --------------------------------------------------------------- */

/* Sizes in the proportions a compiler allocates them: measured as the sizes
 * clangd's allocations fell into over a warm start (most below 128 bytes, a
 * tail to a few kilobytes, a few large). */
static const unsigned g_sizes[64] = {
	16, 16, 16, 24, 24, 24, 32, 32, 32, 32, 40, 40, 48, 48, 48, 56,
	64, 64, 64, 72, 80, 96, 96, 112, 128, 128, 136, 160, 192, 224, 256, 256,
	16, 24, 32, 32, 48, 64, 64, 96, 128, 176, 256, 320, 384, 512, 512, 640,
	768, 1024, 1024, 1536, 2048, 3072, 4096, 4096, 6144, 8192, 16384, 32768, 24, 32, 48, 65536,
};

#define LIVE 256

static void churn(long it, unsigned seed)
{
	void* live[LIVE] = { 0 };
	for (long i = 0; i < it; i++) {
		seed = seed * 1103515245u + 12345u;
		const unsigned k = (seed >> 8) % LIVE;
		free(live[k]);
		live[k] = malloc(g_sizes[(seed >> 16) & 63]);
		((volatile char*)live[k])[0] = 1;
	}
	for (int k = 0; k < LIVE; k++) free(live[k]);
}

struct targ { long it; int threads; };

static void* thread_churn(void* p)
{
	const struct targ* a = p;
	churn(a->it, (unsigned)(uintptr_t)p);
	return 0;
}

static void b_malloc(void* p, long it)
{
	const struct targ* a = p;
	pthread_t t[8];
	struct targ each = { it, a->threads };
	if (a->threads == 1) { churn(it, 7); return; }
	for (int i = 0; i < a->threads; i++) pthread_create(&t[i], 0, thread_churn, &each);
	for (int i = 0; i < a->threads; i++) pthread_join(t[i], 0);
}

static long minor_faults(void)
{
#if defined(__linux__)
	FILE* f = fopen("/proc/self/stat", "r");
	if (!f) return -1;
	char buf[1024];
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	char* p = strrchr(buf, ')');
	if (!p) return -1;
	/* After the name: state, ppid, pgrp, session, tty, tpgid, flags, minflt. */
	long v[8];
	if (sscanf(p + 2, "%*c %ld %ld %ld %ld %ld %ld %ld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7) return -1;
	return v[6];
#else
	return -1;
#endif
}

static void allocation(void)
{
	char name[64];
	static const int threads[] = { 1, 2, 4, 8 };
	for (int i = 0; i < 4; i++) {
		struct targ a = { 0, threads[i] };
		snprintf(name, sizeof name, "malloc/%dt", threads[i]);
		report(name, measure(b_malloc, &a), "ns");
	}
	/* Faults: a gigabyte allocated and released in blocks of the mix above. */
	const long before = minor_faults();
	if (before >= 0) {
		size_t bytes = 0;
		unsigned seed = 3;
		void* live[LIVE] = { 0 };
		while (bytes < ((size_t)1 << 30)) {
			seed = seed * 1103515245u + 12345u;
			const unsigned k = (seed >> 8) % LIVE;
			free(live[k]);
			const size_t sz = g_sizes[(seed >> 16) & 63] * 4;
			live[k] = malloc(sz);
			memset(live[k], 1, sz);
			bytes += sz;
		}
		for (int k = 0; k < LIVE; k++) free(live[k]);
		report("faults/GB", (double)(minor_faults() - before), "faults");
	}
}

/* --- threads ------------------------------------------------------------------ */

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static long g_counter;
static pthread_key_t g_key;

static void b_mutex(void* p, long it)
{
	(void)p;
	for (long i = 0; i < it; i++) { pthread_mutex_lock(&g_mutex); g_counter++; pthread_mutex_unlock(&g_mutex); }
}
static void* thread_mutex(void* p) { b_mutex(0, *(long*)p); return 0; }
static void b_mutex4(void* p, long it)
{
	(void)p;
	pthread_t t[4];
	for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, thread_mutex, &it);
	for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
}
static void b_tsd(void* p, long it)
{
	(void)p;
	for (long i = 0; i < it; i++) KEEP(pthread_getspecific(g_key));
}
static void b_clock(void* p, long it)
{
	(void)p;
	for (long i = 0; i < it; i++) KEEP(now_ns());
}

static void threads(void)
{
	report("mutex/1t", measure(b_mutex, 0), "ns");
	report("mutex/4t", measure(b_mutex4, 0), "ns");
	pthread_key_create(&g_key, 0);
	pthread_setspecific(g_key, &g_key);
	report("tsd", measure(b_tsd, 0), "ns");
	report("clock", measure(b_clock, 0), "ns");
}

/* --- files -------------------------------------------------------------------- */

static char g_path[512];
static int g_fd;

static void b_stat(void* p, long it)
{
	(void)p;
	struct stat st;
	for (long i = 0; i < it; i++) KEEP(stat(g_path, &st));
}
static void b_open(void* p, long it)
{
	(void)p;
	for (long i = 0; i < it; i++) close(open(g_path, O_RDONLY));
}
static void b_pread(void* p, long it)
{
	(void)p;
	char buf[4096];
	for (long i = 0; i < it; i++) KEEP(pread(g_fd, buf, sizeof buf, (off_t)((i * 4096) & ((1 << 20) - 1))));
}
static void* thread_pread(void* p) { b_pread(0, *(long*)p); return 0; }
static void b_pread4(void* p, long it)
{
	(void)p;
	pthread_t t[4];
	for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, thread_pread, &it);
	for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
}
static void* thread_open(void* p) { b_open(0, *(long*)p); return 0; }
static void b_open4(void* p, long it)
{
	(void)p;
	pthread_t t[4];
	for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, thread_open, &it);
	for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
}

static void files(void)
{
	const char* dir = getenv("TMPDIR");
	snprintf(g_path, sizeof g_path, "%s/okm-bench-%ld", dir && *dir ? dir : "/tmp", (long)getpid());
	int fd = open(g_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) { printf("bench files skipped: cannot create %s\n", g_path); return; }
	static char block[1 << 16];
	memset(block, 'f', sizeof block);
	for (int i = 0; i < 16; i++) if (write(fd, block, sizeof block) != (ssize_t)sizeof block) break;
	close(fd);
	g_fd = open(g_path, O_RDONLY);
	report("stat", measure(b_stat, 0), "ns");
	report("open-close/1t", measure(b_open, 0), "ns");
	report("open-close/4t", measure(b_open4, 0), "ns");
	report("pread/1t", measure(b_pread, 0), "ns");
	report("pread/4t", measure(b_pread4, 0), "ns");
	close(g_fd);
	unlink(g_path);
}

int main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--quick") == 0) g_run_ns = 2e6;
	for (size_t i = 0; i < sizeof g_a; i++) g_a[i] = (unsigned char)('a' + i % 23);
	strings();
	allocation();
	threads();
	files();
	printf("-- done --\n");
	return 0;
}
