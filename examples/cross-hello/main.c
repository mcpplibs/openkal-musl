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
	int failures = 0;
	#define check(ok, what) do { \
		printf("%s: %s\n", (ok) ? "ok" : "FAIL", what); \
		if (!(ok)) ++failures; } while (0)

	check(argc >= 1 && argv[0] != NULL, "the program received its own name");

	char* p = malloc(4096);
	check(p != NULL, "an allocation succeeds");
	if (p) { memset(p, 0x5a, 4096); check(p[4095] == 0x5a, "the whole of it is writable"); free(p); }

	FILE* f = fopen("cross-hello.tmp", "w");
	check(f != NULL, "a file is created");
	if (f) {
		check(fprintf(f, "%d", 12345) == 5, "it is written to");
		fclose(f);
		f = fopen("cross-hello.tmp", "r");
		int v = 0;
		check(f && fscanf(f, "%d", &v) == 1 && v == 12345, "it reads back");
		if (f) fclose(f);
		remove("cross-hello.tmp");
	}

	printf("-- failures: %d --\n", failures);
	return failures != 0;
}
