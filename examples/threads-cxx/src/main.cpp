/* A thread created from C++ is the thread that is joined.
 *
 * musl declares `pthread_t' twice: a pointer for C, and `unsigned long' for C++.
 * The two agree wherever a long holds a pointer, which is every architecture
 * musl was written for. Windows is LLP64 --- a long is thirty-two bits --- so a
 * C++ program kept the lower half of the thread's address, and pthread_join read
 * through the truncated value. libc++'s std::thread stores exactly this type,
 * so every std::thread on that system ended in an access violation when joined.
 *
 * THE static_assert IS THE CRITERION, and it is a compile-time one: before the
 * change this file does not compile for x86_64-windows-gnu. The run afterwards
 * shows that the value survives the round trip through a started context.
 */
#include <pthread.h>
#include <stdio.h>

static_assert(sizeof(pthread_t) >= sizeof(void*), "pthread_t must hold the address musl stores in it");

static void* work(void* arg)
{
    *static_cast<int*>(arg) = 42;
    return arg;
}

int main()
{
    int failures = 0;
    int value = 0;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, work, &value) != 0) {
        puts("pthread_create failed");
        return 1;
    }
    void* result = nullptr;
    const int joined = pthread_join(thread, &result);
    printf("joined: %d, value %d, result %s\n", joined, value, result == &value ? "is the argument" : "is not the argument");
    if (joined != 0) ++failures;
    if (value != 42) ++failures;
    if (result != &value) ++failures;
    printf("-- failures: %d --\n", failures);
    return failures == 0 ? 0 : 1;
}
