/* clock_gettime of the two clocks a program reads most, without the system
 * call dispatcher (0.26.0).
 *
 * musl reads a clock through the system's fast path where it has one (the
 * vDSO) and otherwise by a system call; this port has neither, and every read
 * went through okm_syscall.c's dispatcher to `kal_time_monotonic' or
 * `kal_time_wall', which the implementation answers without entering the
 * kernel (openkal-linux 0.21 through the vDSO). The dispatcher was a third of
 * the cost of a read (bench/: 13.5 ns against 10.4 for the system's own).
 * The monotonic clock and the wall clock are read here directly; every other
 * clock takes the dispatcher's path, unchanged. Replaces
 * musl/src/time/clock_gettime.c (mcpp.toml). */
#include "okm.h"

#include <errno.h>
#include <time.h>
#include "syscall.h"

static inline void put(kal_u64 ns, struct timespec* ts)
{
	ts->tv_sec  = (time_t)(ns / 1000000000u);
	ts->tv_nsec = (long)(ns % 1000000000u);
}

int __clock_gettime(clockid_t clk, struct timespec* ts)
{
	switch (clk) {
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_MONOTONIC_COARSE:
	case CLOCK_BOOTTIME:
		put(kal_time_monotonic(), ts);
		return 0;
	case CLOCK_REALTIME:
	case CLOCK_REALTIME_COARSE:
		put(kal_time_wall(), ts);
		return 0;
	default:
		return __syscall_ret(__syscall(SYS_clock_gettime, clk, ts));
	}
}

weak_alias(__clock_gettime, clock_gettime);
