/* Allocation by several threads at once: under the default allocator,
 * mimalloc embedded (0.26.0), whose heaps are per thread; under
 * `malloc-mallocng', each thread's cache of small slots in front of mallocng
 * (port/src/mallocng/okm_malloc_tc.c, 0.25.0). CI runs it with both.
 *
 * WHAT A CACHE COULD GET WRONG IS WHAT IS OBSERVED. Every allocation is filled
 * with a pattern that names it and is checked when it is freed, so a slot handed
 * out twice, or reused while held, is a mismatch; a quarter of the allocations
 * are freed by another thread than the one that made them, so a slot crosses
 * caches; the workers end and are started again, so every cache goes back to
 * mallocng and is made anew; sizes reach past the cached classes, so both paths
 * run side by side; and realloc must keep the bytes it moves. The cost of one
 * allocation and its free with one, two and four threads is printed for the
 * record and not judged: a shared runner's numbers are not a bound. */
#define _GNU_SOURCE
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
static pthread_mutex_t report = PTHREAD_MUTEX_INITIALIZER;
static void check(int ok, const char* what)
{
	pthread_mutex_lock(&report);
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failures++;
	pthread_mutex_unlock(&report);
}

struct block { unsigned char* p; size_t n; unsigned char tag; };

/* A ring the workers pass blocks through, so that one frees what another made. */
#define RING 1024
static struct block ring[RING];
static unsigned ring_head, ring_tail;
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;

static int intact(const struct block* b)
{
	for (size_t i = 0; i < b->n; i++) if (b->p[i] != (unsigned char)(b->tag + i)) return 0;
	return malloc_usable_size(b->p) >= b->n;
}

static void fill(struct block* b)
{
	for (size_t i = 0; i < b->n; i++) b->p[i] = (unsigned char)(b->tag + i);
}

struct worker { unsigned seed; long rounds; long bad; };

static void* work(void* arg)
{
	struct worker* w = arg;
	struct block keep[64] = { { 0 } };
	unsigned s = w->seed;
	for (long i = 0; i < w->rounds; i++) {
		s = s * 1103515245u + 12345u;
		unsigned k = (s >> 8) & 63;
		if (keep[k].p) {
			if (!intact(&keep[k])) w->bad++;
			if ((s >> 20) & 3) free(keep[k].p);
			else {
				pthread_mutex_lock(&ring_lock);
				if (ring_head - ring_tail < RING) { ring[ring_head++ % RING] = keep[k]; keep[k].p = 0; }
				pthread_mutex_unlock(&ring_lock);
				if (keep[k].p) free(keep[k].p);
			}
			keep[k].p = 0;
		}
		/* Mostly small, some past the cached classes (2032 bytes). */
		size_t n = ((s >> 14) & 7) ? 1 + ((s >> 3) & 511) : 1 + ((s >> 3) & 4095);
		keep[k].p = malloc(n);
		keep[k].n = n;
		keep[k].tag = (unsigned char)s;
		if (!keep[k].p) { w->bad++; continue; }
		fill(&keep[k]);
		pthread_mutex_lock(&ring_lock);
		struct block other = { 0 };
		if (ring_head != ring_tail) other = ring[ring_tail++ % RING];
		pthread_mutex_unlock(&ring_lock);
		if (other.p) { if (!intact(&other)) w->bad++; free(other.p); }
	}
	for (int k = 0; k < 64; k++) if (keep[k].p) { if (!intact(&keep[k])) w->bad++; free(keep[k].p); }
	return 0;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void* bench(void* arg)
{
	long n = *(long*)arg;
	void* keep[256] = { 0 };
	unsigned s = (unsigned)(uintptr_t)&n;
	for (long i = 0; i < n; i++) {
		s = s * 1103515245u + 12345u;
		unsigned k = (s >> 8) & 255;
		free(keep[k]);
		keep[k] = malloc(16 + ((s >> 16) & 511));
	}
	for (int k = 0; k < 256; k++) free(keep[k]);
	return 0;
}

int main(void)
{
	/* Three generations of four workers: each generation's caches go back to
	 * mallocng when its threads end. */
	long bad = 0;
	for (int gen = 0; gen < 3; gen++) {
		pthread_t t[4];
		struct worker w[4];
		for (int i = 0; i < 4; i++) {
			w[i] = (struct worker){ .seed = 7919u * (gen * 4 + i + 1), .rounds = 60000, .bad = 0 };
			pthread_create(&t[i], 0, work, &w[i]);
		}
		for (int i = 0; i < 4; i++) { pthread_join(t[i], 0); bad += w[i].bad; }
	}
	while (ring_head != ring_tail) {
		struct block b = ring[ring_tail++ % RING];
		if (!intact(&b)) bad++;
		free(b.p);
	}
	check(bad == 0, "every block four threads made, passed between them and freed held its bytes (three generations)");

	/* realloc of a cached slot keeps its bytes, growing past the cached classes and back. */
	pthread_t helper;
	long one = 1000;
	pthread_create(&helper, 0, bench, &one);   /* the program has a second thread: the caches are in use */
	pthread_join(helper, 0);
	unsigned char* p = malloc(24);
	for (int i = 0; i < 24; i++) p[i] = (unsigned char)i;
	free(malloc(24));
	p = realloc(p, 3000);
	int kept = p != 0;
	for (int i = 0; kept && i < 24; i++) kept = p[i] == i;
	p = realloc(p, 40);
	for (int i = 0; kept && i < 24; i++) kept = p[i] == i;
	free(p);
	check(kept, "realloc keeps the bytes of a slot through a class the caches hold and one they do not");

	void* a = malloc(100);
	free(a);
	void* b = malloc(100);
	check(b != 0 && malloc_usable_size(b) >= 100, "a slot freed and taken again has the size asked for");
	free(b);

	for (int threads = 1; threads <= 4; threads *= 2) {
		long n = 400000;
		pthread_t t[4];
		double t0 = now();
		for (int i = 0; i < threads; i++) pthread_create(&t[i], 0, bench, &n);
		for (int i = 0; i < threads; i++) pthread_join(t[i], 0);
		printf("time %d thread(s): %.1f ns per malloc and free in each\n", threads, (now() - t0) * 1e9 / n);
	}
	printf("-- failures: %d --\n", failures);
	return failures != 0;
}
