/* One call from each of the four archives this fixture links explicitly:
 * `-lm', `-lpthread', `-ldl' and `-lrt'. Every one of the four functions
 * below is defined by musl's own objects, already on this program's link
 * line without any of the four --- so a correct run here is not evidence
 * that the flags were answered correctly, only that they were not answered
 * WRONGLY in a way that breaks the program. The evidence this fixture exists
 * to produce is read from the linker's own trace of the build that produced
 * this binary (see .github/workflows/ci.yml), not from anything below.
 */
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <dlfcn.h>
#include <time.h>

static int failures = 0;
static void check(int ok, const char *what) {
	if (!ok) { printf("FAIL: %s\n", what); failures++; }
	else printf("ok: %s\n", what);
}

static void *thread_fn(void *arg) { return arg; }

int main(void) {
	check(fmax(1.0, 2.0) == 2.0, "fmax");

	pthread_t t;
	void *result = NULL;
	check(pthread_create(&t, NULL, thread_fn, (void *)1) == 0, "pthread_create");
	check(pthread_join(t, &result) == 0 && result == (void *)1, "pthread_join");

	struct timespec ts;
	check(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "clock_gettime");

	/* A statically linked program cannot dlopen anything; musl reports
	 * that rather than mishandling it, and this checks the report. */
	check(dlopen(NULL, RTLD_NOW) == NULL, "dlopen declines on a static binary");

	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
