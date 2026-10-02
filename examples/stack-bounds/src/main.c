/* Where the context that asks actually stands.
 *
 * musl's pthread_getattr_np was wrong here in both directions, and this example
 * is the reading of what replaced it. For the first context musl began at the
 * auxiliary vector, which this port points at a static array, and found a bottom
 * by growing a mapping the dispatcher refuses; for a started one it named the
 * mapping pthread_create allocated, which openkal's kal_task_start supplies the
 * stack instead of. A caller that trusted either walked off the stack it was on.
 *
 * THE PROPERTY IS CONTAINMENT AND NOT A NUMBER. The address of a local is on the
 * calling context's own stack, so it must lie inside the region the call
 * reports --- for the first context, for a started one, and for a context that
 * has used its stack. Lengths and addresses differ between systems and between
 * builds; that the range contains the context that asked is what a caller relies
 * on.
 *
 * AND THE GENERAL CASE IS STILL REFUSED. `pthread_getattr_np(t, ...)' for a
 * thread other than the caller has no answer above openkal, because a context
 * can only be asked about itself. The refusal is asserted here, because a
 * refusal a program can read is what keeps a wrong range from being returned
 * instead.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static int holds(void* base, size_t size, void* here)
{
	const uintptr_t b = (uintptr_t)base;
	const uintptr_t at = (uintptr_t)here;
	return size != 0 && b + size > b && at >= b && at - b < size;
}

struct started {
	int    reported;      /* the return of the enquiry inside the context */
	void*  base;
	size_t size;
	int    contained;     /* whether the region held a local of that context */
};

static void* body(void* arg)
{
	struct started* s = arg;
	char here = 0;

	pthread_attr_t a;
	int e = pthread_getattr_np(pthread_self(), &a);
	void* base = 0;
	size_t size = 0;
	if (e == 0 && pthread_attr_getstack(&a, &base, &size) != 0) e = -1;

	s->reported = e;
	s->base = base;
	s->size = size;
	s->contained = e == 0 && holds(base, size, &here);
	return 0;
}

int main(void)
{
	int failures = 0;

	/* The first context: the region contains the context that asked. */
	char here = 0;
	pthread_attr_t a;
	void* base = 0;
	size_t size = 0;
	int e = pthread_getattr_np(pthread_self(), &a);
	if (e != 0) {
		printf("FAIL: the first context was refused (%d)\n", e);
		++failures;
	} else if (pthread_attr_getstack(&a, &base, &size) != 0) {
		puts("FAIL: the reported attribute was not readable");
		++failures;
	} else if (!holds(base, size, &here)) {
		printf("FAIL: %p is not inside [%p, %p)\n", (void*)&here, base,
		       (void*)((uintptr_t)base + size));
		++failures;
	}
	printf("stack bounds: first e=%d base=%p size=%lu contained=%d\n", e, base,
	       (unsigned long)size, holds(base, size, &here));

	/* A started context: the same, about itself, and not about the first one. */
	struct started s = { 0, 0, 0, 0 };
	pthread_t t;
	if (pthread_create(&t, 0, body, &s) != 0 || pthread_join(t, 0) != 0) {
		puts("FAIL: the thread did not run");
		++failures;
	} else {
		if (s.reported != 0) {
			printf("FAIL: the started context was refused (%d)\n", s.reported);
			++failures;
		}
		if (!s.contained) {
			puts("FAIL: the started context's region is not its own");
			++failures;
		}
		if (s.base == base && s.size == size) {
			puts("FAIL: the started context was told the first context's region");
			++failures;
		}
		printf("stack bounds: started e=%d base=%p size=%lu contained=%d\n",
		       s.reported, s.base, (unsigned long)s.size, s.contained);
	}

	/* Another thread is refused rather than described, and the identifier used
	 * here has already been joined: the port compares it and never reads it, so
	 * a refusal is the only thing this call can produce. */
	pthread_attr_t other;
	int refused = pthread_getattr_np(t, &other) == ENOSYS;
	if (!refused) { puts("FAIL: another thread was answered rather than refused"); ++failures; }
	printf("stack bounds: another thread refused=%d\n", refused);

	printf("-- failures: %d --\n", failures);
	return failures == 0 ? 0 : 1;
}
