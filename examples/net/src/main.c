/* A POSIX program that names no openkal symbol.
 *
 * ⭐ THAT IS THE WHOLE OF THE CRITERION. This package's claim is that a program
 * written for POSIX runs above openkal without being rewritten, and a probe that
 * called `kal_net_connect' to check that `connect' works would be checking the
 * wrong thing. Nothing below is conditional on which implementation is beneath;
 * the same source is what a program in the index would contain.
 *
 * What it exercises is three of the routes openkal 0.8 made possible and this
 * port added: the socket family upon `openkal.net', the datagram family upon
 * `openkal.datagram', and readiness upon `openkal.timeout'. Reported as
 * mcpplibs/openkal-linux#13. The fourth --- duplicating the calling image upon
 * `openkal.space' --- is examples/subprocess, because it is a different subject
 * and its environment's answer may honestly be no.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;

static void check(int ok, const char* what)
{
	if (!ok) { printf("FAIL: %s (errno=%d)\n", what, errno); failures++; }
	else printf("ok: %s\n", what);
}

/* The loopback address, written out. `inet_pton' would be a second thing under
 * test, and this program is about the socket calls. */
static void loopback(struct sockaddr_in* a, unsigned short port)
{
	memset(a, 0, sizeof *a);
	a->sin_family = AF_INET;
	a->sin_port = htons(port);
	a->sin_addr.s_addr = htonl(0x7f000001u);
}

int main(void)
{
	/* Unbuffered, so that a run that stops says how far it got. */
	setbuf(stdout, NULL);

	printf("-- openkal-musl network probe --\n");

	/* --- a listener, and the endpoint the environment chose ----------------- */

	const int lis = socket(AF_INET, SOCK_STREAM, 0);
	check(lis >= 0, "a stream socket is made");
	if (lis < 0) { printf("-- failures: %d --\n", failures); return 1; }

	struct sockaddr_in want;
	loopback(&want, 0);
	check(bind(lis, (struct sockaddr*)&want, sizeof want) == 0, "it is bound to port zero");
	check(listen(lis, 4) == 0, "it listens");

	/* ⭐ PORT ZERO ASKS THE ENVIRONMENT TO CHOOSE, and a program that must
	 * publish where it is listening has no other way to learn it. This is the
	 * enquiry openkal.net added `kal_net_listener_local' for. */
	struct sockaddr_in got;
	socklen_t glen = sizeof got;
	check(getsockname(lis, (struct sockaddr*)&got, &glen) == 0
	      && glen == sizeof got && ntohs(got.sin_port) != 0,
	      "the port it was given is reported back");
	printf("   listening on 127.0.0.1:%u\n", (unsigned)ntohs(got.sin_port));

	/* --- nothing is pending yet --------------------------------------------- */

	struct pollfd pf = { lis, POLLIN, 0 };
	check(poll(&pf, 1, 0) == 0, "polling the listener reports nothing yet");

	check(fcntl(lis, F_SETFL, O_NONBLOCK) == 0, "the listener is made non-blocking");
	errno = 0;
	check(accept(lis, NULL, NULL) < 0 && errno == EAGAIN,
	      "a non-blocking accept with nothing pending reports EAGAIN");
	check(fcntl(lis, F_SETFL, 0) == 0, "and blocking again");

	/* --- a connection ------------------------------------------------------- */

	const int cli = socket(AF_INET, SOCK_STREAM, 0);
	check(cli >= 0, "a second stream socket is made");
	check(connect(cli, (struct sockaddr*)&got, sizeof got) == 0, "it connects");

	pf.revents = 0;
	check(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN),
	      "the listener is now readable");

	struct sockaddr_in from;
	socklen_t flen = sizeof from;
	const int srv = accept(lis, (struct sockaddr*)&from, &flen);
	check(srv >= 0, "the connection is accepted");
	check(from.sin_family == AF_INET && ntohl(from.sin_addr.s_addr) == 0x7f000001u,
	      "and the peer it names is the loopback address");

	struct sockaddr_in peer;
	socklen_t plen = sizeof peer;
	check(getpeername(cli, (struct sockaddr*)&peer, &plen) == 0
	      && ntohs(peer.sin_port) == ntohs(got.sin_port),
	      "the client's peer is the port it connected to");

	/* --- bytes, both ways --------------------------------------------------- */

	check(write(cli, "ping", 4) == 4, "four bytes are written to the connection");

	char in[16];
	memset(in, 0, sizeof in);
	/* ⚠️ READ IN A LOOP. A read of a connection may report fewer bytes than
	 * were sent, on every system --- and this port answers a readiness enquiry
	 * by holding one byte, so a poll before a read makes the short read the
	 * ordinary case rather than a rare one. A program that assumed otherwise
	 * would be asserting something POSIX does not promise. */
	size_t have = 0;
	while (have < 4) {
		const ssize_t r = read(srv, in + have, 4 - have);
		if (r <= 0) break;
		have += (size_t)r;
	}
	check(have == 4 && memcmp(in, "ping", 4) == 0, "and arrive at the other end");

	check(send(srv, "pong", 4, 0) == 4, "four are sent back");
	memset(in, 0, sizeof in);
	have = 0;
	while (have < 4) {
		const ssize_t r = recv(cli, in + have, 4 - have, 0);
		if (r <= 0) break;
		have += (size_t)r;
	}
	check(have == 4 && memcmp(in, "pong", 4) == 0, "and arrive back");

	/* --- half-closure ------------------------------------------------------- */

	check(shutdown(cli, SHUT_WR) == 0, "the client ends its half of the connection");
	const ssize_t eof = read(srv, in, sizeof in);
	check(eof == 0, "and the server observes the end of input");

	check(close(cli) == 0, "the client is closed");
	check(close(srv) == 0, "the server end is closed");
	check(close(lis) == 0, "the listener is closed");

	/* --- datagrams ----------------------------------------------------------- */

	const int a = socket(AF_INET, SOCK_DGRAM, 0);
	const int b = socket(AF_INET, SOCK_DGRAM, 0);
	check(a >= 0 && b >= 0, "two datagram sockets are made");

	struct sockaddr_in abind;
	loopback(&abind, 0);
	check(bind(a, (struct sockaddr*)&abind, sizeof abind) == 0, "one is bound");
	socklen_t alen = sizeof abind;
	check(getsockname(a, (struct sockaddr*)&abind, &alen) == 0 && ntohs(abind.sin_port) != 0,
	      "and reports the port it was given");

	check(sendto(b, "dgram", 5, 0, (struct sockaddr*)&abind, sizeof abind) == 5,
	      "a message is sent to it");

	struct pollfd df = { a, POLLIN, 0 };
	check(poll(&df, 1, 2000) == 1 && (df.revents & POLLIN),
	      "the receiving socket becomes readable");

	char msg[32];
	struct sockaddr_in sender;
	socklen_t slen = sizeof sender;
	memset(msg, 0, sizeof msg);
	const ssize_t got_n = recvfrom(a, msg, sizeof msg, 0, (struct sockaddr*)&sender, &slen);
	/* ⚠️ THE MESSAGE ARRIVES WHOLE, WHICH IS THE PROPERTY THAT DISTINGUISHES A
	 * DATAGRAM FROM A STREAM. A loop here would hide a port that had split it. */
	check(got_n == 5 && memcmp(msg, "dgram", 5) == 0, "the whole message arrives at once");
	check(sender.sin_family == AF_INET && ntohl(sender.sin_addr.s_addr) == 0x7f000001u,
	      "and it names who sent it");

	check(close(a) == 0 && close(b) == 0, "both datagram sockets are closed");

	/* --- select over a set --------------------------------------------------- */

	int pipefd[2];
	check(pipe(pipefd) == 0, "a pipe is made");
	check(write(pipefd[1], "x", 1) == 1, "a byte is put into it");

	fd_set rd;
	FD_ZERO(&rd);
	FD_SET(pipefd[0], &rd);
	struct timeval tv = { 2, 0 };
	check(select(pipefd[0] + 1, &rd, NULL, NULL, &tv) == 1 && FD_ISSET(pipefd[0], &rd),
	      "select reports the read end ready");
	char one = 0;
	check(read(pipefd[0], &one, 1) == 1 && one == 'x', "and the byte is still there to read");
	close(pipefd[0]);
	close(pipefd[1]);

	printf("-- failures: %d --\n", failures);
	return failures ? 1 : 0;
}
