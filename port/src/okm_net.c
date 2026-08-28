/* Sockets, expressed over openkal.net and openkal.datagram.
 *
 * These two interfaces arrived in openkal 0.8 and nothing above them used them
 * until now: a program built on this library reached `socket' and was told
 * ENOSYS, because musl's network sources compile and issue system calls the
 * dispatcher had no case for. Reported as mcpplibs/openkal-linux#13. The gap
 * was in this port and not in musl's sources and not in the specification.
 *
 * ⚠️⚠️ THE ONE STRUCTURAL DIFFERENCE, AND EVERYTHING ELSE FOLLOWS FROM IT.
 *
 * BSD makes a socket first and decides what it is afterwards: `socket' yields
 * an object, and `connect' or `bind'+`listen' then gives it a role. openkal
 * does not have the intermediate object at all --- `kal_net_connect' produces a
 * connection, `kal_net_listen' produces a listener, and there is no operation
 * that produces neither.
 *
 * That is not a deficiency to work around. An unbound socket is a thing whose
 * only capability is to become something else, and clause 3 admits an interface
 * only for a capability that is minimal and universal. What is needed above is
 * the DEFERRAL: a descriptor made by `socket' holds the three numbers it was
 * given and nothing more, and the openkal operation happens at the call that
 * says what the socket is for. `bind' records an endpoint; `listen' spends it;
 * `connect' spends its own.
 *
 * ⇒ Four states, one table, one file. A caller cannot observe the deferral:
 * every error a real kernel would report at `bind' this port reports at
 * `listen', which is one call later and carries the same value.
 *
 * ⚠️⚠️ EVERY REFERENCE TO EITHER INTERFACE IS WEAK, AND THE RULE IS NOT
 * OPTIONAL. Clause 6.1 expresses an interface an implementation does not
 * provide as the ABSENCE of its definitions, and openkal-macos, openkal-windows,
 * openkal-opensbi and openkal-uefi all decline these two today. A strong
 * reference from here would turn "this backend has no network" into "no program
 * above this library links", which is precisely the fault the same rule was
 * added for `kal_process_channel' and `kal_random_fill' to avoid --- twice
 * already, in this same port.
 *
 * ⭐ ONE TEST PER INTERFACE AND NOT ONE PER OPERATION. Clause 3 requires an
 * implementation to provide an interface in whole or not at all, so whether
 * `kal_net_connect' is present answers for all eleven names. Every name is
 * still DECLARED weak --- that is what keeps the link from requiring it --- and
 * exactly one is TESTED.
 */
#define _GNU_SOURCE
#include "okm.h"
#include "okm_opt.h"

#include <openkal/net.h>
#include <openkal/datagram.h>
#include <openkal/timeout.h>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>

/* musl's internal overlay redirects these to its own entry points, and this
 * file wants the public ones, exactly as okm_spawn.c does. */
#undef malloc
#undef calloc
#undef realloc
#undef free

/* --- the interfaces beneath, all weak -------------------------------------- */

extern __typeof(kal_net_connect)         kal_net_connect         __attribute__((__weak__));
extern __typeof(kal_net_listen)          kal_net_listen          __attribute__((__weak__));
extern __typeof(kal_net_accept)          kal_net_accept          __attribute__((__weak__));
extern __typeof(kal_net_stream)          kal_net_stream          __attribute__((__weak__));
extern __typeof(kal_net_peer)            kal_net_peer            __attribute__((__weak__));
extern __typeof(kal_net_local)           kal_net_local           __attribute__((__weak__));
extern __typeof(kal_net_listener_local)  kal_net_listener_local  __attribute__((__weak__));
extern __typeof(kal_net_shutdown)        kal_net_shutdown        __attribute__((__weak__));
extern __typeof(kal_net_close)           kal_net_close           __attribute__((__weak__));
extern __typeof(kal_net_close_listener)  kal_net_close_listener  __attribute__((__weak__));

extern __typeof(kal_datagram_open)      kal_datagram_open      __attribute__((__weak__));
extern __typeof(kal_datagram_local)     kal_datagram_local     __attribute__((__weak__));
extern __typeof(kal_datagram_send_to)   kal_datagram_send_to   __attribute__((__weak__));
extern __typeof(kal_datagram_recv_from) kal_datagram_recv_from __attribute__((__weak__));
extern __typeof(kal_datagram_close)     kal_datagram_close     __attribute__((__weak__));

extern __typeof(kal_timeout_accept)    kal_timeout_accept    __attribute__((__weak__));
extern __typeof(kal_timeout_recv_from) kal_timeout_recv_from __attribute__((__weak__));

#define OKM_HAVE_NET   (kal_net_connect   != 0)
#define OKM_HAVE_DGRAM (kal_datagram_open != 0)

/* --- the table -------------------------------------------------------------- */

enum {
	OKM_SOCK_FREE = 0,
	OKM_SOCK_NEW,      /* made; no openkal object yet                          */
	OKM_SOCK_LISTEN,   /* kal_net_listen has been performed                    */
	OKM_SOCK_CONN,     /* kal_net_connect, or a connection kal_net_accept gave */
	OKM_SOCK_DGRAM,    /* kal_datagram_open has been performed                 */
};

/* The largest message this port will hold on a datagram socket's behalf, which
 * is the largest a datagram carries. A readiness enquiry has to RECEIVE the
 * message to know there is one --- openkal has no enquiry that reports a
 * message without taking it --- so the buffer is what makes the enquiry
 * non-destructive. It is obtained when a datagram socket is first polled and
 * not before, because a program that never polls never needs it. */
#define OKM_DGRAM_MAX 65536

struct okm_sock {
	int state;
	int domain, type, protocol;
	int have_local;                 /* `bind' recorded an endpoint            */
	int have_peer;                  /* `connect' recorded one, for a datagram */
	struct kal_endpoint local, peer;
	struct kal_net_conn     conn;
	struct kal_net_listener lis;
	struct kal_datagram     dg;
	kal_u64 rcv_bound, snd_bound;   /* SO_RCVTIMEO / SO_SNDTIMEO, nanoseconds */

	/* What a readiness enquiry took and has not yet delivered. */
	int pend_conn;
	struct kal_net_conn pend;
	int pend_msg;
	unsigned long pend_len;
	struct kal_endpoint pend_from;
	unsigned char* buf;
};

static struct okm_sock g_sock[OKM_MAX_SOCK];

static int slot_alloc(void)
{
	for (int i = 0; i < OKM_MAX_SOCK; i++)
		if (g_sock[i].state == OKM_SOCK_FREE) {
			struct okm_sock* s = &g_sock[i];
			unsigned char* keep = s->buf;      /* the buffer outlives a slot */
			for (unsigned k = 0; k < sizeof *s; k++) ((char*)s)[k] = 0;
			s->buf = keep;
			s->state = OKM_SOCK_NEW;
			return i;
		}
	return -1;
}

static struct okm_sock* slot_of(struct okm_desc* d)
{
	if (!d || d->kind != OKM_SOCKET) return 0;
	if (d->sock < 0 || d->sock >= OKM_MAX_SOCK) return 0;
	return &g_sock[d->sock];
}

void okm_sock_release(int slot)
{
	if (slot < 0 || slot >= OKM_MAX_SOCK) return;
	struct okm_sock* s = &g_sock[slot];
	switch (s->state) {
	case OKM_SOCK_CONN:   if (kal_net_close)          kal_net_close(s->conn);        break;
	case OKM_SOCK_LISTEN: if (kal_net_close_listener) kal_net_close_listener(s->lis); break;
	case OKM_SOCK_DGRAM:  if (kal_datagram_close)     kal_datagram_close(s->dg);      break;
	default: break;
	}
	/* A connection accepted to answer a readiness enquiry and never taken is
	 * still a connection, and closing the listener does not close it. */
	if (s->pend_conn && kal_net_close) kal_net_close(s->pend);
	s->state = OKM_SOCK_FREE;
	s->pend_conn = 0; s->pend_msg = 0;
	/* ⚠️ THE BUFFER IS KEPT AND THE SLOT IS NOT. Freeing it here would return
	 * memory to an allocator this library also implements, from a path a
	 * program may reach while holding the table's lock. It is at most
	 * OKM_DGRAM_MAX per slot, the slot count is fixed, and the next socket to
	 * occupy the slot reuses it. */
}

/* --- addresses -------------------------------------------------------------- */

/* An endpoint is address bytes in network order plus a port in host order, and
 * a `sockaddr' is the same two in this system's own layout. The conversion is
 * written here rather than taken from `inet_pton' because it is a conversion
 * between two structures and not a parse of text.
 *
 * ⚠️ A LENGTH OR A FAMILY THIS PORT DOES NOT KNOW IS REFUSED RATHER THAN READ
 * AS ONE IT DOES, which is the rule the specification states for the same
 * conversion in the other direction: an implementation that ignored the field
 * would misread every address a later revision defines, silently. */
static int to_endpoint(const void* addr, unsigned len, struct kal_endpoint* out)
{
	if (!addr || !out) return -EFAULT;
	for (unsigned i = 0; i < sizeof out->addr; i++) out->addr[i] = 0;
	out->addr_len = 0;
	out->port = 0;

	const struct sockaddr* sa = (const struct sockaddr*)addr;
	if (sa->sa_family == AF_INET) {
		if (len < sizeof(struct sockaddr_in)) return -EINVAL;
		const struct sockaddr_in* v4 = (const struct sockaddr_in*)addr;
		const unsigned char* b = (const unsigned char*)&v4->sin_addr.s_addr;
		for (int i = 0; i < 4; i++) out->addr[i] = b[i];
		out->addr_len = 4;
		out->port = (kal_u32)ntohs(v4->sin_port);
		return 0;
	}
	if (sa->sa_family == AF_INET6) {
		if (len < sizeof(struct sockaddr_in6)) return -EINVAL;
		const struct sockaddr_in6* v6 = (const struct sockaddr_in6*)addr;
		for (int i = 0; i < 16; i++) out->addr[i] = v6->sin6_addr.s6_addr[i];
		/* Twenty bytes is the address followed by a scope identifier, and the
		 * shorter form denotes the same address when the scope is zero. Sending
		 * the shorter one where it suffices keeps an address that arrived as
		 * sixteen bytes going back out as sixteen. */
		if (v6->sin6_scope_id != 0) {
			const kal_u32 sc = v6->sin6_scope_id;
			for (int i = 0; i < 4; i++)
				out->addr[16 + i] = (kal_u8)((sc >> (i * 8)) & 0xffu);
			out->addr_len = 20;
		} else {
			out->addr_len = 16;
		}
		out->port = (kal_u32)ntohs(v6->sin6_port);
		return 0;
	}
	/* AF_UNIX and everything else. openkal names an endpoint by address and
	 * port and has no other form, so a family this port cannot express is
	 * refused at the point of the attempt. */
	return -EAFNOSUPPORT;
}

/* Writes an endpoint into a caller's `sockaddr', truncating as `accept' and
 * `getsockname' are specified to, and reporting the length the whole address
 * would have taken. */
static int from_endpoint(const struct kal_endpoint* ep, void* addr, unsigned* len)
{
	if (!addr || !len) return 0;      /* both optional, together and apart */

	union { struct sockaddr_in v4; struct sockaddr_in6 v6; } u;
	unsigned whole;

	for (unsigned i = 0; i < sizeof u; i++) ((char*)&u)[i] = 0;

	if (ep->addr_len == 4) {
		u.v4.sin_family = AF_INET;
		u.v4.sin_port = htons((unsigned short)ep->port);
		unsigned char* b = (unsigned char*)&u.v4.sin_addr.s_addr;
		for (int i = 0; i < 4; i++) b[i] = ep->addr[i];
		whole = sizeof u.v4;
	} else if (ep->addr_len == 16 || ep->addr_len == 20) {
		u.v6.sin6_family = AF_INET6;
		u.v6.sin6_port = htons((unsigned short)ep->port);
		for (int i = 0; i < 16; i++) u.v6.sin6_addr.s6_addr[i] = ep->addr[i];
		if (ep->addr_len == 20) {
			kal_u32 sc = 0;
			for (int i = 0; i < 4; i++) sc |= (kal_u32)ep->addr[16 + i] << (i * 8);
			u.v6.sin6_scope_id = sc;
		}
		whole = sizeof u.v6;
	} else {
		/* An implementation reported a length this port does not know. The
		 * transfer that produced it still happened; what is unknown is who the
		 * other end was, and the caller is told that rather than given bytes. */
		*len = 0;
		return -EAFNOSUPPORT;
	}

	unsigned room = *len;
	if (room > whole) room = whole;
	for (unsigned i = 0; i < room; i++) ((char*)addr)[i] = ((const char*)&u)[i];
	*len = whole;
	return 0;
}

/* --- the calls -------------------------------------------------------------- */

int okm_sock_open(int domain, int type, int protocol)
{
	if (domain != AF_INET && domain != AF_INET6) return -EAFNOSUPPORT;

	const int base = type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (base != SOCK_STREAM && base != SOCK_DGRAM) return -ESOCKTNOSUPPORT;
	if (base == SOCK_STREAM && !OKM_HAVE_NET)   return -EAFNOSUPPORT;
	if (base == SOCK_DGRAM  && !OKM_HAVE_DGRAM) return -EAFNOSUPPORT;

	/* ⚠️ REFUSED HERE RATHER THAN IGNORED. A descriptor that was asked to be
	 * non-blocking and is not would make every subsequent operation block where
	 * the caller arranged not to, and it would do so silently. */
	if ((type & SOCK_NONBLOCK) && !okm_can_bound()) return -ENOSYS;

	okm_lock();
	const int fd = okm_fd_alloc(0);
	if (fd < 0) { okm_unlock(); return fd; }
	const int slot = slot_alloc();
	if (slot < 0) { okm_unlock(); return -ENFILE; }

	struct kal_file nof = { 0 };
	struct kal_dir  nod = { 0 };
	int flags = O_RDWR;
	if (type & SOCK_NONBLOCK) flags |= O_NONBLOCK;
	if (type & SOCK_CLOEXEC)  flags |= O_CLOEXEC;
	okm_fd_bind(fd, OKM_SOCKET, 0, nof, nod, flags);

	struct okm_desc* d = okm_desc_of(fd);
	d->sock = slot;
	g_sock[slot].domain   = domain;
	g_sock[slot].type     = base;
	g_sock[slot].protocol = protocol;
	okm_unlock();
	return fd;
}

int okm_sock_bind(int fd, const void* addr, unsigned len)
{
	struct okm_desc* d = okm_desc_of(fd);
	struct okm_sock* s = slot_of(d);
	if (!s) return d ? -ENOTSOCK : -EBADF;
	if (s->state != OKM_SOCK_NEW) return -EINVAL;

	struct kal_endpoint ep;
	const int r = to_endpoint(addr, len, &ep);
	if (r) return r;

	/* ⭐ RECORDED AND NOT PERFORMED, which is the deferral this file exists for.
	 * A datagram socket spends it at the first send or receive; a stream socket
	 * spends it at `listen'. Neither loses an error: openkal reports at the
	 * operation what a kernel reports at the bind, and the value is the same
	 * one call later. */
	s->local = ep;
	s->have_local = 1;

	if (s->type == SOCK_DGRAM) {
		if (!OKM_HAVE_DGRAM) return -ENOSYS;
		const int e = kal_datagram_open(&ep, &s->dg);
		if (e != kal_ok) return -okm_errno(e);
		s->state = OKM_SOCK_DGRAM;
	}
	return 0;
}

int okm_sock_listen(int fd, int backlog)
{
	(void)backlog;   /* openkal names no depth and a caller cannot state one */
	struct okm_desc* d = okm_desc_of(fd);
	struct okm_sock* s = slot_of(d);
	if (!s) return d ? -ENOTSOCK : -EBADF;
	if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
	if (s->state == OKM_SOCK_LISTEN) return 0;
	if (s->state != OKM_SOCK_NEW) return -EINVAL;
	if (!OKM_HAVE_NET) return -ENOSYS;

	/* A `listen' without a `bind' asks the environment to choose everything.
	 * openkal has no such form --- an endpoint is required --- so the wildcard
	 * address and port zero are named, which is what the absent bind meant. */
	struct kal_endpoint ep = s->local;
	if (!s->have_local) {
		for (unsigned i = 0; i < sizeof ep.addr; i++) ep.addr[i] = 0;
		ep.addr_len = (s->domain == AF_INET6) ? 16 : 4;
		ep.port = 0;
	}

	const int e = kal_net_listen(&ep, &s->lis);
	if (e != kal_ok) return -okm_errno(e);
	s->state = OKM_SOCK_LISTEN;
	return 0;
}

/* Places a connection into a descriptor of its own. The lock is held. */
static int adopt(struct kal_net_conn c, int flags)
{
	const int fd = okm_fd_alloc(0);
	if (fd < 0) { kal_net_close(c); return fd; }
	const int slot = slot_alloc();
	if (slot < 0) { kal_net_close(c); return -ENFILE; }

	struct kal_file nof = { 0 };
	struct kal_dir  nod = { 0 };
	okm_fd_bind(fd, OKM_SOCKET, kal_net_stream(c).h, nof, nod, O_RDWR | flags);
	struct okm_desc* nd = okm_desc_of(fd);
	nd->sock = slot;
	g_sock[slot].state = OKM_SOCK_CONN;
	g_sock[slot].conn  = c;
	g_sock[slot].type  = SOCK_STREAM;
	return fd;
}

int okm_sock_accept(int fd, void* addr, unsigned* len, int flags)
{
	struct okm_desc* d = okm_desc_of(fd);
	struct okm_sock* s = slot_of(d);
	if (!s) return d ? -ENOTSOCK : -EBADF;
	if (s->state != OKM_SOCK_LISTEN) return -EINVAL;
	if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) return -EINVAL;
	if ((flags & SOCK_NONBLOCK) && !okm_can_bound()) return -ENOSYS;

	struct kal_net_conn c;
	if (s->pend_conn) {
		/* A readiness enquiry already accepted one. Delivering it here is what
		 * makes that enquiry's answer true rather than momentary. */
		c = s->pend;
		s->pend_conn = 0;
	} else if (d->flags & O_NONBLOCK) {
		if (!kal_timeout_accept) return -ENOSYS;
		const int e = kal_timeout_accept(s->lis, OKM_NOW_NS, &c);
		if (e == kal_err_again) return -EAGAIN;
		if (e != kal_ok) return -okm_errno(e);
	} else {
		const int e = kal_net_accept(s->lis, &c);
		if (e != kal_ok) return -okm_errno(e);
	}

	int newflags = 0;
	if (flags & SOCK_NONBLOCK) newflags |= O_NONBLOCK;
	if (flags & SOCK_CLOEXEC)  newflags |= O_CLOEXEC;

	okm_lock();
	const int nfd = adopt(c, newflags);
	if (nfd >= 0 && (flags & SOCK_CLOEXEC)) okm_fd_cloexec(nfd, 1);
	okm_unlock();
	if (nfd < 0) return nfd;

	if (addr && len) {
		struct kal_endpoint peer;
		if (kal_net_peer(c, &peer) == kal_ok) from_endpoint(&peer, addr, len);
		else *len = 0;
	}
	return nfd;
}

int okm_sock_connect(int fd, const void* addr, unsigned len)
{
	struct okm_desc* d = okm_desc_of(fd);
	struct okm_sock* s = slot_of(d);
	if (!s) return d ? -ENOTSOCK : -EBADF;

	struct kal_endpoint ep;
	const int r = to_endpoint(addr, len, &ep);
	if (r) return r;

	if (s->type == SOCK_DGRAM) {
		/* On a datagram socket `connect' names a default peer and sends
		 * nothing. The endpoint is recorded and the socket is opened if `bind'
		 * had not already opened it. */
		if (!OKM_HAVE_DGRAM) return -ENOSYS;
		if (s->state == OKM_SOCK_NEW) {
			const int e = kal_datagram_open(s->have_local ? &s->local : 0, &s->dg);
			if (e != kal_ok) return -okm_errno(e);
			s->state = OKM_SOCK_DGRAM;
		}
		s->peer = ep;
		s->have_peer = 1;
		return 0;
	}

	if (s->state == OKM_SOCK_CONN) return -EISCONN;
	if (s->state != OKM_SOCK_NEW)  return -EINVAL;
	if (!OKM_HAVE_NET) return -ENOSYS;

	/* ⚠️ THE CONNECTION IS ESTABLISHED BEFORE THIS RETURNS, EVEN ON A
	 * NON-BLOCKING DESCRIPTOR, and a caller cannot be told otherwise honestly.
	 *
	 * `kal_net_connect' completes or fails; openkal has no form that begins a
	 * connection and reports its outcome later, and clause 6.3 records
	 * readiness notification among the mechanisms considered and not adopted.
	 * A port that returned EINPROGRESS here would be promising a completion
	 * that nothing can report --- a caller would then poll for POLLOUT, be told
	 * ready, ask SO_ERROR and be told zero, all of it invented.
	 *
	 * So the call blocks and reports the truth. What a non-blocking caller
	 * loses is the overlap, not the outcome; what it would have lost the other
	 * way is the outcome itself. Recorded in musl/PATCHES.md. */
	const int e = kal_net_connect(&ep, &s->conn);
	if (e != kal_ok) return -okm_errno(e);
	s->state = OKM_SOCK_CONN;
	s->peer = ep;
	s->have_peer = 1;
	d->stream = kal_net_stream(s->conn).h;
	return 0;
}

int okm_sock_name(int fd, void* addr, unsigned* len, int peer)
{
	struct okm_desc* d = okm_desc_of(fd);
	struct okm_sock* s = slot_of(d);
	if (!s) return d ? -ENOTSOCK : -EBADF;
	if (!addr || !len) return -EFAULT;

	struct kal_endpoint ep;
	int e = kal_err_not_supported;

	if (peer) {
		if (s->state == OKM_SOCK_CONN)       e = kal_net_peer(s->conn, &ep);
		else if (s->have_peer)             { ep = s->peer; e = kal_ok; }
		else                                 return -ENOTCONN;
	} else {
		switch (s->state) {
		case OKM_SOCK_CONN:   e = kal_net_local(s->conn, &ep); break;
		case OKM_SOCK_LISTEN: e = kal_net_listener_local(s->lis, &ep); break;
		case OKM_SOCK_DGRAM:  e = kal_datagram_local(s->dg, &ep); break;
		default:
			/* Nothing has been opened, so there is nothing the environment has
			 * assigned. What `bind' recorded is the whole of the answer, and a
			 * socket that has not been bound either has none. */
			if (!s->have_local) return -EINVAL;
			ep = s->local;
			e = kal_ok;
			break;
		}
	}
	if (e != kal_ok) return -okm_errno(e);
	return from_endpoint(&ep, addr, len);
}

int okm_sock_shutdown(int fd, int how)
{
	struct okm_desc* d = okm_desc_of(fd);
	struct okm_sock* s = slot_of(d);
	if (!s) return d ? -ENOTSOCK : -EBADF;
	if (s->state != OKM_SOCK_CONN) return -ENOTCONN;

	/* This system numbers the directions from zero and openkal from one, so the
	 * mapping is written out rather than computed: a number neither defines
	 * would otherwise be read as SHUT_RD. */
	int dir;
	switch (how) {
	case SHUT_RD:   dir = KAL_SHUT_READ;  break;
	case SHUT_WR:   dir = KAL_SHUT_WRITE; break;
	case SHUT_RDWR: dir = KAL_SHUT_BOTH;  break;
	default: return -EINVAL;
	}
	const int e = kal_net_shutdown(s->conn, dir);
	return e == kal_ok ? 0 : -okm_errno(e);
}

/* Which bound an operation on this descriptor carries: the one a socket option
 * asked for, or the smallest there is when the descriptor is non-blocking, or
 * none. Zero is openkal's spelling of "no bound". */
static kal_u64 bound_of(struct okm_desc* d, struct okm_sock* s, int flags, int receiving)
{
	if ((flags & MSG_DONTWAIT) || (d->flags & O_NONBLOCK)) return OKM_NOW_NS;
	return receiving ? s->rcv_bound : s->snd_bound;
}

long okm_sock_send(int fd, const void* buf, unsigned long len, int flags,
                   const void* addr, unsigned alen)
{
	struct okm_desc* d = okm_desc_of(fd);
	struct okm_sock* s = slot_of(d);
	if (!s) return d ? -ENOTSOCK : -EBADF;

	/* MSG_NOSIGNAL asks that a signal not be raised. There are no signals here,
	 * so the request is satisfied by there being nothing to raise --- the same
	 * shape as masking a set of signals that cannot arrive. Every other flag
	 * names a facility openkal does not have. */
	const int rest = flags & ~(MSG_NOSIGNAL | MSG_DONTWAIT);
	if (rest) return -ENOSYS;

	if (s->type == SOCK_DGRAM) {
		if (!OKM_HAVE_DGRAM) return -ENOSYS;
		struct kal_endpoint to;
		if (addr) {
			const int r = to_endpoint(addr, alen, &to);
			if (r) return r;
		} else if (s->have_peer) {
			to = s->peer;
		} else {
			return -EDESTADDRREQ;
		}
		if (s->state == OKM_SOCK_NEW) {
			const int e = kal_datagram_open(s->have_local ? &s->local : 0, &s->dg);
			if (e != kal_ok) return -okm_errno(e);
			s->state = OKM_SOCK_DGRAM;
		}
		const kal_intptr io = kal_datagram_send_to(s->dg, buf, len, &to);
		if (io < 0) return -okm_errno((int)-io);
		return (long)io;
	}

	if (s->state != OKM_SOCK_CONN) return -ENOTCONN;
	if (addr) return -EISCONN;
	const kal_u64 ns = bound_of(d, s, flags, 0);
	if (ns) return okm_timed_write(d->stream, buf, len, ns);

	struct kal_stream st; st.h = d->stream;
	const kal_intptr io = kal_stream_write(st, buf, len);
	if (io < 0) return -okm_errno((int)-io);
	return (long)io;
}

long okm_sock_recv(int fd, void* buf, unsigned long len, int flags,
                   void* addr, unsigned* alen)
{
	struct okm_desc* d = okm_desc_of(fd);
	struct okm_sock* s = slot_of(d);
	if (!s) return d ? -ENOTSOCK : -EBADF;

	const int rest = flags & ~(MSG_NOSIGNAL | MSG_DONTWAIT);
	if (rest) return -ENOSYS;   /* MSG_PEEK, MSG_OOB, MSG_WAITALL, MSG_TRUNC */

	if (s->type == SOCK_DGRAM) {
		if (!OKM_HAVE_DGRAM) return -ENOSYS;
		if (s->state == OKM_SOCK_NEW) {
			const int e = kal_datagram_open(s->have_local ? &s->local : 0, &s->dg);
			if (e != kal_ok) return -okm_errno(e);
			s->state = OKM_SOCK_DGRAM;
		}

		struct kal_endpoint from;
		unsigned long got;

		if (s->pend_msg) {
			/* The message a readiness enquiry took. Excess beyond the caller's
			 * buffer is lost, which is what the medium does and what this
			 * interface states. */
			got = s->pend_len < len ? s->pend_len : len;
			for (unsigned long i = 0; i < got; i++) ((char*)buf)[i] = (char)s->buf[i];
			from = s->pend_from;
			s->pend_msg = 0;
		} else {
			const kal_u64 ns = bound_of(d, s, flags, 1);
			kal_intptr io;
			if (ns) {
				if (!kal_timeout_recv_from) return -ENOSYS;
				io = kal_timeout_recv_from(s->dg, buf, len, &from, ns);
				if (io == -kal_err_again) return -EAGAIN;
			} else {
				io = kal_datagram_recv_from(s->dg, buf, len, &from);
			}
			if (io < 0) return -okm_errno((int)-io);
			got = (unsigned long)io;
		}
		if (addr && alen) from_endpoint(&from, addr, alen);
		return (long)got;
	}

	if (s->state != OKM_SOCK_CONN) return -ENOTCONN;
	if (addr && alen) *alen = 0;

	const long held = okm_take_ahead(d, buf, len);
	if (held == OKM_AHEAD_EOF) return 0;
	if (held) return held;

	const kal_u64 ns = bound_of(d, s, flags, 1);
	if (ns) return okm_timed_read(d->stream, buf, len, ns);

	struct kal_stream st; st.h = d->stream;
	const kal_intptr io = kal_stream_read(st, buf, len);
	if (io < 0) return -okm_errno((int)-io);
	return (long)io;
}

/* --- options ----------------------------------------------------------------
 *
 * ⚠️ AN OPTION THIS PORT CANNOT HONOUR IS REFUSED. Accepting one and ignoring it
 * is the single failure this whole port is written to avoid: a caller that set
 * SO_BROADCAST and was told it succeeded would send to a broadcast address and
 * be told the send succeeded too, and nothing would ever arrive. ENOPROTOOPT is
 * the value this system already defines for an option a socket does not have,
 * and every caller of `setsockopt' handles a failure.
 *
 * Three are honoured because openkal makes them true, and one is accepted
 * because it is ALREADY true --- which is a different statement from ignoring
 * it, and is recorded where it is made. */

static kal_u64 ns_of_timeval(const void* val, unsigned len)
{
	if (len < sizeof(struct timeval)) return (kal_u64)-1;
	const struct timeval* tv = (const struct timeval*)val;
	if (tv->tv_sec == 0 && tv->tv_usec == 0) return 0;    /* no bound */
	kal_u64 ns = (kal_u64)tv->tv_sec * 1000000000ull + (kal_u64)tv->tv_usec * 1000ull;
	return ns ? ns : OKM_NOW_NS;
}

int okm_sock_setopt(int fd, int level, int opt, const void* val, unsigned len)
{
	struct okm_desc* d = okm_desc_of(fd);
	struct okm_sock* s = slot_of(d);
	if (!s) return d ? -ENOTSOCK : -EBADF;
	if (level != SOL_SOCKET) return -ENOPROTOOPT;

	switch (opt) {
	case SO_REUSEADDR:
		/* ⭐ ACCEPTED BECAUSE IT IS ALREADY IN EFFECT, not because it is
		 * harmless. openkal's `kal_net_listen' sets this on the listener it
		 * makes --- openkal-linux/src/net.cpp says so and gives the reason: a
		 * program restarted within the kernel's lingering interval is the
		 * ordinary case. A caller asking for it is asking for what it has. */
		return 0;
	case SO_RCVTIMEO:
	case SO_SNDTIMEO: {
		const kal_u64 ns = ns_of_timeval(val, len);
		if (ns == (kal_u64)-1) return -EINVAL;
		if (ns && !okm_can_bound()) return -ENOSYS;
		if (opt == SO_RCVTIMEO) s->rcv_bound = ns; else s->snd_bound = ns;
		return 0;
	}
	default:
		return -ENOPROTOOPT;
	}
}

int okm_sock_getopt(int fd, int level, int opt, void* val, unsigned* len)
{
	struct okm_desc* d = okm_desc_of(fd);
	struct okm_sock* s = slot_of(d);
	if (!s) return d ? -ENOTSOCK : -EBADF;
	if (level != SOL_SOCKET) return -ENOPROTOOPT;
	if (!val || !len) return -EFAULT;

	switch (opt) {
	case SO_TYPE:
		if (*len < sizeof(int)) return -EINVAL;
		*(int*)val = s->type;
		*len = sizeof(int);
		return 0;
	case SO_ERROR:
		/* ⭐ ALWAYS ZERO, AND IT IS AN ACCURATE ANSWER RATHER THAN A STAND-IN.
		 * This option reports an error that arrived after the call that would
		 * have reported it returned. Every operation here completes before it
		 * returns --- see the note at `connect' --- so there is never one
		 * outstanding, and zero is what "no error is pending" means. */
		if (*len < sizeof(int)) return -EINVAL;
		*(int*)val = 0;
		*len = sizeof(int);
		return 0;
	case SO_RCVTIMEO:
	case SO_SNDTIMEO: {
		if (*len < sizeof(struct timeval)) return -EINVAL;
		const kal_u64 ns = (opt == SO_RCVTIMEO) ? s->rcv_bound : s->snd_bound;
		struct timeval* tv = (struct timeval*)val;
		tv->tv_sec  = (time_t)(ns / 1000000000ull);
		tv->tv_usec = (suseconds_t)((ns % 1000000000ull) / 1000ull);
		*len = sizeof *tv;
		return 0;
	}
	default:
		return -ENOPROTOOPT;
	}
}

/* --- readiness --------------------------------------------------------------- */

int okm_sock_shape(struct okm_desc* d)
{
	struct okm_sock* s = slot_of(d);
	if (!s) return OKM_SOCK_SHAPE_IDLE;
	if (s->state == OKM_SOCK_CONN) return OKM_SOCK_SHAPE_STREAM;
	if (s->state == OKM_SOCK_LISTEN || s->state == OKM_SOCK_DGRAM)
		return OKM_SOCK_SHAPE_OWN;
	return OKM_SOCK_SHAPE_IDLE;
}

int okm_sock_wait_in(struct okm_desc* d, kal_u64 ns)
{
	struct okm_sock* s = slot_of(d);
	if (!s) return -EBADF;

	if (s->state == OKM_SOCK_LISTEN) {
		if (s->pend_conn) return 1;
		if (!kal_timeout_accept) return -ENOSYS;
		struct kal_net_conn c;
		const int e = kal_timeout_accept(s->lis, ns, &c);
		if (e == kal_err_again) return 0;
		if (e != kal_ok) return -okm_errno(e);
		s->pend = c;
		s->pend_conn = 1;
		return 1;
	}

	if (s->state == OKM_SOCK_DGRAM) {
		if (s->pend_msg) return 1;
		if (!kal_timeout_recv_from) return -ENOSYS;
		/* ⚠️ THE BUFFER IS WHY A READINESS ENQUIRY IS NOT DESTRUCTIVE HERE.
		 * openkal reports a message by delivering it, so the only way to learn
		 * that one has arrived is to take it, and the only way to keep the
		 * enquiry honest is to hold it until the receive that follows. */
		if (!s->buf) {
			s->buf = (unsigned char*)malloc(OKM_DGRAM_MAX);
			if (!s->buf) return -ENOMEM;
		}
		struct kal_endpoint from;
		const kal_intptr io =
			kal_timeout_recv_from(s->dg, s->buf, OKM_DGRAM_MAX, &from, ns);
		if (io == -kal_err_again) return 0;
		if (io < 0) return -okm_errno((int)-io);
		s->pend_len  = (unsigned long)io;
		s->pend_from = from;
		s->pend_msg  = 1;
		return 1;
	}

	return 0;   /* nothing can arrive on a socket that is neither */
}
