/* An ordinary POSIX program.
 *
 * The source contains nothing about openkal. It opens a file by a global path,
 * reads it, consults an environment variable and reports three counts, using
 * the interfaces a C program has used for forty years. That it runs at all is
 * the claim this package makes; that its three counts equal the system's own
 * `wc' is how the claim is tested, because a program that merely produces
 * output proves only that it produced output.
 *
 * The previous version of this program was written against a module this
 * package exported, and it could not have been compared against `wc' because
 * it was not the same kind of program. This one is.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <file>\n", argv[0]);
		return 2;
	}

	const int verbose = getenv("WORDCOUNT_VERBOSE") != NULL;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }

	unsigned long lines = 0, words = 0, bytes = 0;
	int in_word = 0, c;
	while ((c = fgetc(f)) != EOF) {
		bytes++;
		if (c == '\n') lines++;
		if (isspace(c)) in_word = 0;
		else if (!in_word) { in_word = 1; words++; }
	}
	fclose(f);

	printf("lines %lu words %lu bytes %lu\n", lines, words, bytes);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (verbose) {
		const long long ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000
		                   + (t1.tv_nsec - t0.tv_nsec);
		fprintf(stderr, "elapsed %lld nanoseconds\n", ns);
	}
	return 0;
}
