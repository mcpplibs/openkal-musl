/* A large allocation is a mapping, and a mapping is whole pages.
 *
 * musl's allocator obtains an allocation of MMAP_THRESHOLD (131,052) bytes or
 * more as a mapping of its own, and uses the mapping up to the end of its last
 * page: the block may start up to a page into it, and the slot's footer sits
 * just below that end. The port obtained exactly the length asked for, so on
 * Windows, where that length comes from the process heap, the footer and up to
 * a page of the block lay past the end of what was obtained --- over the next
 * heap block's header. The program went on until the heap next walked there.
 *
 * WHAT IS OBSERVED.
 *   (1) An anonymous mapping of a length that ends inside a page: the rest of
 *       that page reads as zero and can be written, and memory allocated beside
 *       it keeps its contents.
 *   (2) Blocks from half the threshold to eight megabytes, each grown from the
 *       one before the way a string grows, filled end to end, checked, and
 *       released while the next is held --- lengths on a page and inside one.
 *   (3) realloc across the threshold in both directions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { PAGE = 4096 };

static int failures;

static void fail(const char* what, size_t n)
{
	printf("FAIL: %s (%zu)\n", what, n);
	++failures;
}

static int filled_with(const unsigned char* p, size_t n, unsigned char value)
{
	for (size_t i = 0; i < n; ++i)
		if (p[i] != value) return 0;
	return 1;
}

static void mapping_tail(void)
{
	const size_t len = PAGE + 904;   /* ends inside the second page */
	unsigned char* neighbours[16];
	for (int i = 0; i < 16; ++i) {
		neighbours[i] = malloc(64);
		if (neighbours[i]) memset(neighbours[i], 0x5a, 64);
	}
	unsigned char* m = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (m == MAP_FAILED) { fail("mmap", len); return; }
	for (int i = 0; i < 16; ++i) {
		unsigned char* after = malloc(64);
		if (after) { memset(after, 0x5a, 64); free(after); }
	}
	if (!filled_with(m, 2 * PAGE, 0)) fail("the last page of a mapping reads as zero", 2 * PAGE);
	memset(m, 0xa5, 2 * PAGE);
	for (int i = 0; i < 16; ++i)
		if (neighbours[i] && !filled_with(neighbours[i], 64, 0x5a)) fail("a block allocated before the mapping kept its contents", 64);
	if (munmap(m, len) != 0) fail("munmap", len);
	for (int i = 0; i < 16; ++i) free(neighbours[i]);
	printf("mapping: %zu bytes asked, %d readable and writable\n", len, 2 * PAGE);
}

static void growth(void)
{
	unsigned char* held = 0;
	size_t held_size = 0;
	int blocks = 0;
	for (size_t size = 65536; size <= (size_t)8 << 20; size *= 2) {
		for (size_t inside = 0; inside < 3; ++inside) {
			const size_t n = size + inside * 1021;
			unsigned char* p = malloc(n);
			if (!p) { fail("malloc", n); continue; }
			const unsigned char value = (unsigned char)(n % 251);
			if (held) {
				if (!filled_with(held, held_size, (unsigned char)(held_size % 251))) fail("the held block kept its contents", held_size);
				memcpy(p, held, held_size);
			}
			memset(p + held_size, value, n - held_size);
			memset(p, value, held_size);
			/* Small allocations walk the heap beside the large ones. */
			for (int i = 0; i < 32; ++i) {
				unsigned char* small = malloc(24 + i);
				if (small) { memset(small, 0x33, 24 + i); free(small); }
			}
			if (!filled_with(p, n, value)) fail("a block kept its contents", n);
			free(held);
			held = p;
			held_size = n;
			++blocks;
		}
	}
	free(held);
	printf("growth: %d blocks up to %zu bytes\n", blocks, held_size);
}

static void reallocation(void)
{
	size_t n = 1000;
	unsigned char* p = malloc(n);
	if (!p) { fail("malloc", n); return; }
	memset(p, 0x11, n);
	const size_t sizes[] = { 140000, 600000, 131052, 131051, 2000, 300000, 4096 * 64 + 1, 100 };
	for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
		const size_t m = sizes[i];
		unsigned char* q = realloc(p, m);
		if (!q) { fail("realloc", m); free(p); return; }
		const size_t kept = n < m ? n : m;
		if (!filled_with(q, kept, 0x11)) fail("realloc kept the contents", m);
		memset(q, 0x11, m);
		p = q;
		n = m;
	}
	free(p);
	printf("realloc: %zu sizes across the threshold\n", sizeof sizes / sizeof sizes[0]);
}

int main(void)
{
	mapping_tail();
	growth();
	reallocation();
	printf("-- failures: %d --\n", failures);
	return failures == 0 ? 0 : 1;
}
