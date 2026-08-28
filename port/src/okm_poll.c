/* Readiness, and transfers bounded in time.
 *
 * ⚠️⚠️ openkal HAS NO OPERATION THAT REPORTS WHETHER A TRANSFER WOULD PROCEED,
 * AND THAT IS DELIBERATE. Clause 6.3 records readiness notification among the
 * mechanisms considered and NOT adopted: an interface reporting readiness
 * obliges every implementation of it to maintain a set and a context of its
 * own, which is a mechanism reconstructed rather than a facility conveyed. What
 * openkal has instead is `openkal.timeout' --- the same operations with a bound
 * added --- and openkal-linux's own timeout.cpp states the consequence in one
 * line: "a bounded read is a bounded wait for readiness followed by the
 * ordinary read".
 *
 * ⭐ SO `poll' IS ANSWERED BY ATTEMPTING THE TRANSFER AND KEEPING WHAT IT
 * PRODUCED. One byte from a stream, one connection from a listener, one message
 * from a datagram endpoint --- held in the descriptor and delivered to the
 * operation that follows. POLLIN asserts that a read will not block, and a byte
 * already in hand is the strongest form that assertion can take: it is true
 * when the enquiry is answered and still true when the read happens, which a
 * report about the state of a queue is not.
 *
 * This port already answers `getdents' the same way, and for the same reason:
 * openkal has no operation that returns an entry to an iterator, so an entry
 * that does not fit the caller's buffer is held rather than lost.
 *
 * ⚠️ WHAT IS NOT FAITHFUL, STATED HERE RATHER THAN DISCOVERED.
 *
 *   POLLOUT is reported for every descriptor that may be written, without an
 *   enquiry, because there is nothing to enquire of. A bounded write bounds the
 *   WAIT and not the TRANSFER --- openkal/include/openkal/timeout.h says so in
 *   terms --- so a write this port begins completes or reports, and there is no
 *   state in which it "would block" that openkal can be asked about. A program
 *   that polls for POLLOUT to avoid blocking may therefore block in the write.
 *   Recorded in musl/PATCHES.md.
 *
 *   POLLPRI, POLLRDBAND and the rest name out-of-band data, which openkal does
 *   not have. They are never reported.
 *
 *   A set larger than one descriptor is waited upon by asking each in turn
 *   under the smallest bound there is, and repeating. That is O(n) per round
 *   and is what an interface without a readiness set permits; `epoll' remains
 *   withheld, being a facility of one kernel rather than a capability.
 */
#define _GNU_SOURCE
#include "okm.h"
#include "okm_opt.h"

#include <openkal/timeout.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>

/* Weak, by the rule okm_net.c states: `openkal.timeout' is optional, and a
 * strong reference would make a backend that declines it one no program above
 * this library could link against. */
extern __typeof(kal_timeout_read)  kal_timeout_read  __attribute__((__weak__));
extern __typeof(kal_timeout_write) kal_timeout_write __attribute__((__weak__));

int okm_can_bound(void) { return kal_timeout_read != 0; }

/* --- bounded transfer -------------------------------------------------------- */

long okm_timed_read(kal_uintptr stream, void* buf, unsigned long len, kal_u64 ns)
{
	if (!kal_timeout_read) return -ENOSYS;
	if (len == 0) return 0;
	struct kal_stream s; s.h = stream;
	const kal_intptr io = kal_timeout_read(s, buf, len, ns);
	if (io < 0) return io == -kal_err_again ? -EAGAIN : -okm_errno((int)-io);
	return (long)io;
}

long okm_timed_write(kal_uintptr stream, const void* buf, unsigned long len, kal_u64 ns)
{
	if (!kal_timeout_write) return -ENOSYS;
	if (len == 0) return 0;
	struct kal_stream s; s.h = stream;
	const kal_intptr io = kal_timeout_write(s, buf, len, ns);
	/* The count, or the condition when nothing moved --- which is what this
	 * code used to compute from the pair by hand at every site. */
	if (io < 0) return io == -kal_err_again ? -EAGAIN : -okm_errno((int)-io);
	return (long)io;
}

/* --- the read-ahead ---------------------------------------------------------- */

long okm_take_ahead(struct okm_desc* d, void* buf, unsigned long len)
{
	if (!d) return 0;
	if (d->ahead && len) {
		((unsigned char*)buf)[0] = d->ahead_byte;
		d->ahead = 0;
		return 1;
	}
	if (d->ahead_eof) { d->ahead_eof = 0; return OKM_AHEAD_EOF; }
	return 0;
}

/* Whether a descriptor can have input at all, and by which route. */
static int has_input_route(struct okm_desc* d)
{
	switch (d->kind) {
	case OKM_STREAM:
	case OKM_CHANNEL:
		return (d->flags & O_ACCMODE) != O_WRONLY;
	case OKM_SOCKET:
		return okm_sock_shape(d) != OKM_SOCK_SHAPE_IDLE;
	default:
		return 0;
	}
}

/* Waits up to `ns' for a descriptor to have input, and keeps what arrived.
 * 1 ready, 0 the bound expired, negative a negated errno value. */
static int wait_in(struct okm_desc* d, kal_u64 ns)
{
	if (d->ahead || d->ahead_eof) return 1;

	if (d->kind == OKM_SOCKET && okm_sock_shape(d) == OKM_SOCK_SHAPE_OWN)
		return okm_sock_wait_in(d, ns);

	if (!kal_timeout_read) return -ENOSYS;

	unsigned char byte = 0;
	struct kal_stream s; s.h = d->stream;
	const kal_intptr io = kal_timeout_read(s, &byte, 1, ns);
	if (io == -kal_err_again) return 0;
	if (io < 0) return -okm_errno((int)-io);
	if (io == 0) {
		/* End of input. A read will return zero without waiting, which is
		 * exactly what POLLIN asserts, so the descriptor is ready and stays
		 * ready --- the flag is not cleared by the read that observes it. */
		d->ahead_eof = 1;
		return 1;
	}
	d->ahead = 1;
	d->ahead_byte = byte;
	return 1;
}

/* --- poll -------------------------------------------------------------------- */

/* The interval a set of more than one descriptor is revisited at. It is the
 * granularity openkal-linux reports for its own bounded operations, and the
 * interval its `kal_timeout_wait_process' polls a child at; matching it means
 * this loop costs what that one costs rather than adding a second figure. */
#define OKM_ROUND_NS 1000000ull

static void sleep_ns(kal_u64 ns)
{
	struct timespec ts;
	ts.tv_sec  = (time_t)(ns / 1000000000ull);
	ts.tv_nsec = (long)(ns % 1000000000ull);
	nanosleep(&ts, 0);
}

long okm_poll(void* p, unsigned long n, int timeout_ms)
{
	struct pollfd* fds = (struct pollfd*)p;
	if (n && !fds) return -EFAULT;
	if (n > OKM_MAX_FD) return -EINVAL;

	const int forever = timeout_ms < 0;
	kal_u64 left = forever ? 0 : (kal_u64)timeout_ms * 1000000ull;

	/* ⭐ A SET OF ONE IS THE CASE WORTH SEPARATING, AND IT IS THE COMMON ONE.
	 * With a single descriptor the caller's whole bound can be handed to the
	 * one bounded operation, so the wait happens in the environment rather than
	 * in this loop. Only a larger set has to be revisited. */
	const int single = (n == 1) && (fds[0].fd >= 0);

	for (;;) {
		long ready = 0;
		long failure = 0;
		int  waited = 0;

		for (unsigned long i = 0; i < n; i++) {
			fds[i].revents = 0;
			if (fds[i].fd < 0) continue;

			struct okm_desc* d = okm_desc_of(fds[i].fd);
			if (!d) { fds[i].revents = POLLNVAL; ready++; continue; }

			/* A file and a directory are always ready, which is what every
			 * environment reports for them: a read from a file does not wait. */
			if (d->kind == OKM_FILE || d->kind == OKM_DIR) {
				if (fds[i].events & POLLIN)  fds[i].revents |= POLLIN;
				if (fds[i].events & POLLOUT) fds[i].revents |= POLLOUT;
				if (fds[i].revents) ready++;
				continue;
			}

			if ((fds[i].events & POLLIN) && has_input_route(d)) {
				/* ⚠️⚠️ ZERO MEANS THE OPPOSITE IN THE TWO INTERFACES, AND THE
				 * COLLISION IS SILENT.
				 *
				 * `poll' spells "do not wait" as a timeout of zero; openkal
				 * spells "wait without end" as a bound of zero --- timeout.h
				 * says so, and `kal_task_wait' established the convention
				 * before it. Passing the caller's zero straight through turns
				 * the one call that must not wait into the one that never
				 * returns. Measured: `poll(&pf, 1, 0)' on an idle listener
				 * hung, and the probe printed four lines and stopped.
				 *
				 * ⇒ The smallest bound there is, which is not zero. What it
				 * costs is one granularity of the environment beneath --- a
				 * millisecond on openkal-linux --- and what it buys is that a
				 * bound always means a bound. */
				const kal_u64 ns = single ? (forever ? 0 : (left ? left : OKM_NOW_NS))
				                          : OKM_NOW_NS;
				waited = 1;
				const int r = wait_in(d, ns);
				if (r < 0) { if (!failure) failure = r; }
				else if (r > 0) {
					fds[i].revents |= POLLIN;
					/* End of input on a channel or a connection is a hang-up as
					 * well as a readable state, and a program that reads until
					 * POLLHUP is the ordinary shape. A file has no hang-up. */
					if (d->ahead_eof && d->kind != OKM_STREAM)
						fds[i].revents |= POLLHUP;
				}
			}

			/* Writability, reported without an enquiry. The head of this file
			 * states why there is none to make. */
			if ((fds[i].events & POLLOUT) && (d->flags & O_ACCMODE) != O_RDONLY)
				fds[i].revents |= POLLOUT;

			if (fds[i].revents) ready++;
		}

		if (ready) return ready;
		/* ⚠️ A FAILURE IS REPORTED ONLY WHEN NOTHING WAS READY. A descriptor
		 * whose environment refused the enquiry must not hide the readiness of
		 * the others in the same set. */
		if (failure) return failure;
		if (!forever && left == 0) return 0;

		/* The single descriptor was handed the caller's whole bound, so the
		 * wait has already happened in the environment and expiring here is the
		 * answer. Where nothing was waited upon --- a set of one that cannot
		 * receive, or one asking only about writability it was already told
		 * about --- the bound is still owed to the caller and is spent below. */
		if (single && waited) {
			if (!forever) return 0;
			continue;
		}

		if (!forever) {
			if (left <= OKM_ROUND_NS) left = 0;
			else left -= OKM_ROUND_NS;
		}
		sleep_ns(OKM_ROUND_NS);
	}
}
