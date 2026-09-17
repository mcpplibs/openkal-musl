/* A detached thread ends, and the program that started it goes on.
 *
 * musl releases a detached thread's mapping from inside the thread, and on Linux
 * the thread is standing on that mapping --- so `__unmapself' first moves to a
 * 256-byte stack every exiting thread shares, and makes the two system calls
 * that end the thread from there. In this port neither call is a system call:
 * each passes through the port's dispatcher, the context table and openkal,
 * and an unoptimized build of that path needs several times 256 bytes. The
 * overflow wrote over whatever the linker placed beneath the shared stack. On
 * macOS that is musl's table of thread-specific keys and this port's context
 * table, and the thread then looked up its own record in the table it had just
 * overwritten and jumped into it: every program whose detached thread ended
 * stopped with an access violation.
 *
 * WHAT IS OBSERVED. Detached threads that end one after another, then the
 * two tables beneath the shared stack in use: a key with a destructor, and a
 * joinable thread's own error value, which is reached through the context table.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

enum { DETACHED = 8 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reported = PTHREAD_COND_INITIALIZER;
static int started;
static int ended;

static void* detached(void* arg)
{
	(void)arg;
	pthread_mutex_lock(&lock);
	++ended;
	pthread_cond_broadcast(&reported);
	pthread_mutex_unlock(&lock);
	return 0;
}

static int destroyed;
static void destroy(void* value) { if (value == &destroyed) ++destroyed; }

static pthread_key_t key;
static void* keyed(void* arg)
{
	(void)arg;
	errno = 0;
	pthread_setspecific(key, &destroyed);
	errno = 7;
	return (void*)(long)errno;
}

static void pause_briefly(void)
{
	struct timespec ts = { 0, 50 * 1000 * 1000 };
	nanosleep(&ts, 0);
}

int main(void)
{
	int failures = 0;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	for (int i = 0; i < DETACHED; ++i) {
		pthread_t thread;
		if (pthread_create(&thread, &attr, detached, 0) != 0) {
			printf("FAIL: detached thread %d was not created\n", i);
			++failures;
			break;
		}
		++started;
		pthread_mutex_lock(&lock);
		while (ended < started) pthread_cond_wait(&reported, &lock);
		pthread_mutex_unlock(&lock);
		/* Reporting precedes ending; the pause lets the thread finish ending
		 * before the next one starts, which is where the fault was. */
		pause_briefly();
	}
	pthread_attr_destroy(&attr);
	printf("detached: %d started, %d ended\n", started, ended);
	if (ended != DETACHED) { printf("FAIL: %d of %d detached threads ended\n", ended, DETACHED); ++failures; }

	if (pthread_key_create(&key, destroy) != 0) { puts("FAIL: pthread_key_create"); ++failures; }
	pthread_t joinable;
	void* result = 0;
	if (pthread_create(&joinable, 0, keyed, 0) != 0 || pthread_join(joinable, &result) != 0) {
		puts("FAIL: the joinable thread did not run");
		++failures;
	}
	printf("joinable: its error value %ld, destructor ran %d time(s)\n", (long)result, destroyed);
	if ((long)result != 7) { puts("FAIL: the joinable thread's error value"); ++failures; }
	if (destroyed != 1) { puts("FAIL: the key's destructor"); ++failures; }

	printf("-- failures: %d --\n", failures);
	return failures == 0 ? 0 : 1;
}
