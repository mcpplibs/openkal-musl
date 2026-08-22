/* A program built on one system and started on another.
 *
 * It is deliberately not a "hello": what it prints has to be something the
 * machine that runs it can check, and what is worth checking is that the C
 * library came up --- that streams work, that the environment was received,
 * that allocation works. A program that printed a constant would prove only
 * that control reached its first statement. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
	/* ⚠️ UNBUFFERED, AND THAT IS PART OF THE PROBE.
	 *
	 * Output to a pipe is fully buffered, so a program that dies part way
	 * through loses everything it had printed --- and what the machine that
	 * runs this reports is then not where the program got to. Measured: one run
	 * showed a single line and stopped, and the line it showed was not the last
	 * one it had executed. A probe whose report is buffered is a probe that
	 * lies about where it stopped. */
	setvbuf(stdout, NULL, _IONBF, 0);

	int failures = 0;
	/* ⚠️ THE CONDITION IS EVALUATED ONCE, AND THE FIRST VERSION DID NOT.
	 *
	 * It read `(ok) ? "ok" : "FAIL"' and then `if (!(ok))', so an argument with
	 * an effect happened twice --- and one of the arguments below is
	 * `fclose(f) == 0'. The second close was of a stream already closed, and
	 * the program died at the NEXT fopen with a segmentation fault.
	 *
	 * It was measured on a machine of the other system first, where it looked
	 * like a defect in this port, and it reproduced on this one --- which is
	 * what said it was not. A probe that has an effect in its own condition is
	 * a probe that tests itself as well as its subject. */
	#define check(ok, what) do { \
		const int okm_ok_ = !!(ok); \
		printf("%s: %s\n", okm_ok_ ? "ok" : "FAIL", what); \
		if (!okm_ok_) ++failures; } while (0)

	check(argc >= 1 && argv[0] != NULL, "the program received its own name");

	char* p = malloc(4096);
	check(p != NULL, "an allocation succeeds");
	if (p) { memset(p, 0x5a, 4096); check(p[4095] == 0x5a, "the whole of it is writable"); free(p); }

	FILE* f = fopen("cross-hello.tmp", "w");
	check(f != NULL, "a file is created");
	if (f) {
		check(fprintf(f, "%d", 12345) == 5, "it is written to");
		check(fclose(f) == 0, "it closes");
		f = fopen("cross-hello.tmp", "r");
		check(f != NULL, "it opens again for reading");
		if (f) {
			/* Split, because a single assertion over three operations names
			 * none of them when it fails --- which is what the first version
			 * of this program did, on a machine this one cannot run. */
			int v = 0;
			int n = fscanf(f, "%d", &v);
			printf("   (fscanf returned %d, value %d)\n", n, v);
			check(n == 1, "a formatted read succeeds");
			check(v == 12345, "it reads back what was written");
			fclose(f);
		}
		remove("cross-hello.tmp");
	}

	printf("-- failures: %d --\n", failures);
	return failures != 0;
}
