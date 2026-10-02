/* pthread_getattr_np reports that it cannot say, for the first context and for a started one.
 *
 * openkal reports no bounds for the stack a context runs on. The range musl
 * would compute is a page of the port's static auxiliary vector for the first
 * context, and for a started one the mapping pthread_create allocated and the
 * context never runs on; a caller that trusts either walks off the real stack.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>

static void* body(void* arg)
{
	pthread_attr_t a;
	*(int*)arg = pthread_getattr_np(pthread_self(), &a);
	return 0;
}

int main(void)
{
	int failures = 0;
	pthread_attr_t a;
	int first = pthread_getattr_np(pthread_self(), &a);
	int started = 0;
	pthread_t t;
	if (pthread_create(&t, 0, body, &started) != 0 || pthread_join(t, 0) != 0) {
		puts("FAIL: the thread did not run");
		++failures;
	}
	printf("stack bounds: first %d, started %d\n", first, started);
	if (first != ENOSYS) { puts("FAIL: the first context's stack bounds"); ++failures; }
	if (started != ENOSYS) { puts("FAIL: a started context's stack bounds"); ++failures; }
	printf("-- failures: %d --\n", failures);
	return failures == 0 ? 0 : 1;
}
