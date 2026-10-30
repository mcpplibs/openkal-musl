/* A file's contents in the address space: mmap, munmap and msync of a file
 * over openkal.map (openkal 0.18, port/src/okm_map.c).
 *
 * THE BYTES ARE THE OBSERVATION. A pointer that came back and a call that
 * reported success are what a port returning plausible values would also give,
 * so each case reads the mapping, or writes it and reads the file with read(2).
 *
 * The offsets POSIX allows and openkal does not are the port's to reconstruct:
 * a page-aligned offset inside a 64 KiB alignment, and a length past the end
 * of the file. A request for an executable file mapping is refused (EACCES).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures;
static void check(int ok, const char* what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failures++;
}

static unsigned char pattern(size_t i) { return (unsigned char)((i * 7 + 3) % 251 + 1); }

int main(void)
{
	const char* name = "okm-mmap-probe.tmp";
	const size_t size = 3 * 65536 + 1000;
	int fd = open(name, O_RDWR | O_CREAT | O_TRUNC, 0644);
	check(fd >= 0, "a file is created");
	if (fd < 0) return 1;
	static unsigned char block[4096];
	for (size_t at = 0; at < size; at += sizeof block) {
		const size_t n = size - at < sizeof block ? size - at : sizeof block;
		for (size_t i = 0; i < n; i++) block[i] = pattern(at + i);
		if (write(fd, block, n) != (ssize_t)n) { check(0, "the file is written"); return 1; }
	}

	/* Read, private, the whole file: the bytes, and zero after the end. */
	unsigned char* m = mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0);
	check(m != MAP_FAILED, "the file is mapped");
	if (m != MAP_FAILED) {
		int same = 1;
		for (size_t i = 0; i < size && same; i++) same = m[i] == pattern(i);
		check(same, "the mapping holds the file's bytes");
		check(m[size] == 0, "the byte after the file's end reads zero");
		check(munmap(m, size) == 0, "the mapping is released");
	}

	/* An offset of one page: inside the openkal alignment, reconstructed here. */
	unsigned char* p = mmap(0, 8192, PROT_READ, MAP_PRIVATE, fd, 4096);
	check(p != MAP_FAILED, "a page-aligned offset is mapped");
	if (p != MAP_FAILED) {
		check(p[0] == pattern(4096) && p[8191] == pattern(4096 + 8191), "it holds the bytes from that offset");
		check(munmap(p, 8192) == 0, "it is released");
	}

	/* A length past the end, as POSIX allows: the file's part is there. */
	unsigned char* t = mmap(0, 65536, PROT_READ, MAP_PRIVATE, fd, 3 * 65536);
	check(t != MAP_FAILED && t[999] == pattern(3 * 65536 + 999), "a length past the end maps the file's part");
	if (t != MAP_FAILED) munmap(t, 65536);

	/* Shared and written: msync, then read(2) sees it. */
	unsigned char* s = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check(s != MAP_FAILED, "a shared writable mapping is made");
	if (s != MAP_FAILED) {
		const unsigned char v = (unsigned char)~pattern(70000);
		s[70000] = v;
		check(msync(s, size, MS_SYNC) == 0, "msync of a file mapping succeeds");
		unsigned char got = 0;
		check(pread(fd, &got, 1, 70000) == 1 && got == v, "read(2) sees the shared write after msync");
		check(munmap(s, size) == 0, "the shared mapping is released");
	}

	/* Private and written: the file is unchanged. */
	unsigned char* c = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (c != MAP_FAILED) {
		c[10] = 0;
		unsigned char got = 0;
		check(pread(fd, &got, 1, 10) == 1 && got == pattern(10), "a private write does not reach the file");
		munmap(c, size);
	} else check(0, "a private writable mapping is made");

	/* Refused. */
	errno = 0;
	check(mmap(0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0) == MAP_FAILED && errno == EACCES,
	      "an executable file mapping is refused with EACCES");
	int pfd[2];
	if (pipe(pfd) == 0) {
		errno = 0;
		check(mmap(0, 4096, PROT_READ, MAP_PRIVATE, pfd[0], 0) == MAP_FAILED && errno == ENODEV,
		      "a pipe is not mapped (ENODEV)");
		close(pfd[0]); close(pfd[1]);
	}

	close(fd);
	unlink(name);
	printf("-- failures: %d --\n", failures);
	return failures != 0;
}
