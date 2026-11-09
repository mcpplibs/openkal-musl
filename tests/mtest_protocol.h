/* The mtest test protocol, version 1, written by a C program: the probes'
 * observations and the bench's measurements as events a runner reads.
 *
 * WHY C, AND NOT mtest's LIBRARY. This package is a C library, and its probes
 * and its bench are programs above it. mtest's library is C++ that imports
 * `std', and the only C++ runtime over this C library is openkal-llvm-runtime's
 * libc++, which is built above this package: a C library's own tests cannot sit
 * on a runtime built over that library without inverting the layering the
 * manifest states (`requires = ["mcpp:kernel-abi=openkal"]'). So the probes
 * speak the protocol themselves, as openkal's conformance suite does in C++
 * without a runtime (openkal/conformance/src/protocol.cpp, of which this is a
 * translation). The UTest form of what runs above this library is in
 * openkal-llvm-runtime's tests.
 *
 * WHAT IS SPOKEN (the clauses of mtest's protocol):
 *   P2   the handshake: a runner sets MTEST_PROTOCOL=1 and names MTEST_REPORT,
 *        a file the program appends its events to; MTEST_MODE=list asks for
 *        the cases and runs none; without both variables the program runs as
 *        it always has and writes no event. The marker `mtest-protocol=1;' is
 *        in the program's bytes (P2.3).
 *   P3.4 one case per program, of kind `cts', and one part per observation,
 *        `<case>/<observation>', with ` #n' where an observation's text recurs.
 *   P4   MTEST_SELECT, a file of identifiers one per line: a line names the
 *        case or a part of it; a line that names nothing here is `missing'.
 *   P5   one event per line, one write per event.
 *   P6   a measurement is a `metric' event of the running case.
 *   P7.1 every case and part holds `serial:openkal-musl-probe': the probes
 *        create fixed names in the working directory they were given.
 *
 * Usage, a probe: mtp_begin(case) first in `main', mtp_observe(what, held)
 * where it states an observation. The bench: mtp_begin_program() first, and
 * mtp_measure(case, value, unit) for each measured primitive, which is a case
 * of kind `bench' holding one metric. The `done' event is written at exit. */
#ifndef OKM_MTEST_PROTOCOL_H
#define OKM_MTEST_PROTOCOL_H

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MTP_FRAMEWORK "openkal-musl-probe"
#define MTP_VERSION   "0.29.0"
#define MTP_RESOURCE  "serial:openkal-musl-probe"
/* Every function is static: a program includes this once, and uses part of it. */
#define MTP_FN static __attribute__((unused))

/* THE MARKER (P2.3), read once in mtp_begin so that a link that keeps only
 * what it reaches keeps it. */
static const char mtp_marker[] = "mtest-protocol=1;";

static int      mtp_fd = -1;
static int      mtp_listing;
static uint64_t mtp_start_ns;
static char     mtp_case[64];
static int      mtp_case_failed;
static int      mtp_case_open;
static int      mtp_counts[3];          /* pass, fail, skip */
static char*    mtp_select;
static size_t   mtp_select_len;
static int      mtp_selected = 1;       /* whether this program's case runs */
#define MTP_MAX_PARTS 1024
static uint64_t mtp_part_hash[MTP_MAX_PARTS];
static int      mtp_part_seen[MTP_MAX_PARTS];
static int      mtp_parts;

struct mtp_line { char buf[4096]; size_t n; int overflow; };

MTP_FN void mtp_raw(struct mtp_line* l, const char* s, size_t len) {
	for (size_t i = 0; i < len; i++) {
		if (l->n + 1 >= sizeof l->buf) { l->overflow = 1; return; }
		l->buf[l->n++] = s[i];
	}
}
MTP_FN void mtp_cstr(struct mtp_line* l, const char* s) { mtp_raw(l, s, strlen(s)); }
MTP_FN void mtp_str(struct mtp_line* l, const char* s, size_t len) {
	static const char hex[] = "0123456789abcdef";
	mtp_cstr(l, "\"");
	for (size_t i = 0; i < len; i++) {
		const unsigned char c = (unsigned char)s[i];
		if (c == '"')       mtp_cstr(l, "\\\"");
		else if (c == '\\') mtp_cstr(l, "\\\\");
		else if (c == '\n') mtp_cstr(l, "\\n");
		else if (c == '\r') mtp_cstr(l, "\\r");
		else if (c == '\t') mtp_cstr(l, "\\t");
		else if (c < 0x20) {
			const char e[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
			mtp_raw(l, e, 6);
		} else mtp_raw(l, (const char*)&s[i], 1);
	}
	mtp_cstr(l, "\"");
}
MTP_FN void mtp_num(struct mtp_line* l, uint64_t v) {
	char d[24]; int i = 24;
	if (v == 0) d[--i] = '0';
	while (v) { d[--i] = (char)('0' + v % 10); v /= 10; }
	mtp_raw(l, d + i, (size_t)(24 - i));
}
MTP_FN void mtp_send(struct mtp_line* l) {
	if (mtp_fd < 0 || l->overflow) return;
	mtp_cstr(l, "\n");
	ssize_t r = write(mtp_fd, l->buf, l->n);   /* one write per event (P5) */
	(void)r;
}

MTP_FN uint64_t mtp_now_ns(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
MTP_FN uint64_t mtp_ms(void) { return (mtp_now_ns() - mtp_start_ns) / 1000000u; }

MTP_FN uint64_t mtp_hash(const char* s) {
	uint64_t h = 1469598103934665603ull;
	for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
	return h;
}

/* Calls `each' with every non-empty line of the selection. */
MTP_FN void mtp_lines(void (*each)(const char*, size_t, void*), void* ctx) {
	size_t at = 0;
	while (at < mtp_select_len) {
		size_t end = at;
		while (end < mtp_select_len && mtp_select[end] != '\n') end++;
		size_t len = end - at;
		if (len > 0 && mtp_select[at + len - 1] == '\r') len--;
		if (len > 0) each(mtp_select + at, len, ctx);
		at = end + 1;
	}
}

/* A line names a case, or a part within it (`<case>/<what>'). */
MTP_FN int mtp_names(const char* id, const char* s, size_t len) {
	const size_t k = strlen(id);
	return (len == k || (len > k && s[k] == '/')) && memcmp(s, id, k) == 0;
}
struct mtp_query { const char* id; int found; };
MTP_FN void mtp_any_line(const char* s, size_t len, void* ctx) {
	struct mtp_query* q = (struct mtp_query*)ctx;
	if (mtp_names(q->id, s, len)) q->found = 1;
}
/* Whether the runner's selection names a case (every case, without one). */
MTP_FN int mtp_wanted(const char* id) {
	if (!mtp_select) return 1;
	struct mtp_query q; q.id = id; q.found = 0;
	mtp_lines(mtp_any_line, &q);
	return q.found;
}
/* The cases this program ran, so that a selected line naming none of them is
 * reported `missing' (P4). */
#define MTP_MAX_RAN 1024
static uint64_t mtp_ran[MTP_MAX_RAN];
static int      mtp_ran_count;
MTP_FN void mtp_missing_line(const char* s, size_t len, void* ctx) {
	(void)ctx;
	size_t k = 0;
	while (k < len && s[k] != '/') k++;
	char head[256];
	if (k >= sizeof head) k = sizeof head - 1;
	memcpy(head, s, k); head[k] = 0;
	const uint64_t h = mtp_hash(head);
	for (int i = 0; i < mtp_ran_count; i++) if (mtp_ran[i] == h) return;
	struct mtp_line e; e.n = 0; e.overflow = 0;
	mtp_cstr(&e, "{\"e\":\"missing\",\"id\":"); mtp_str(&e, s, len); mtp_cstr(&e, "}"); mtp_send(&e);
}
MTP_FN void mtp_record_ran(const char* id) {
	if (mtp_ran_count < MTP_MAX_RAN) mtp_ran[mtp_ran_count++] = mtp_hash(id);
}

MTP_FN void mtp_end_event(const char* id, const char* status, const char* reason, uint64_t ms) {
	struct mtp_line e; e.n = 0; e.overflow = 0;
	mtp_cstr(&e, "{\"e\":\"end\",\"id\":"); mtp_str(&e, id, strlen(id));
	mtp_cstr(&e, ",\"status\":"); mtp_str(&e, status, strlen(status));
	mtp_cstr(&e, ",\"ms\":"); mtp_num(&e, ms);
	if (reason) { mtp_cstr(&e, ",\"reason\":"); mtp_str(&e, reason, strlen(reason)); }
	mtp_cstr(&e, "}"); mtp_send(&e);
}

/* A `case' (listed) or `begin' event of a case of kind `cts' or `bench'. */
MTP_FN void mtp_case_event(const char* kind_event, const char* id, const char* kind) {
	struct mtp_line e; e.n = 0; e.overflow = 0;
	mtp_cstr(&e, "{\"e\":\""); mtp_cstr(&e, kind_event); mtp_cstr(&e, "\",\"id\":");
	mtp_str(&e, id, strlen(id));
	if (kind_event[0] == 'c') { mtp_cstr(&e, ",\"name\":"); mtp_str(&e, id, strlen(id)); }
	else { mtp_cstr(&e, ",\"t\":"); mtp_num(&e, mtp_ms()); }
	mtp_cstr(&e, ",\"kind\":\""); mtp_cstr(&e, kind); mtp_cstr(&e, "\",\"clauses\":[");
	mtp_str(&e, id, strlen(id));
	mtp_cstr(&e, "],\"resources\":[\"" MTP_RESOURCE "\"]}");
	mtp_send(&e);
}

/* The `done' event, at exit: the case's end, the selection's missing lines
 * and the counts. */
MTP_FN void mtp_finish(void) {
	if (mtp_fd < 0) return;
	if (mtp_case_open)
		mtp_end_event(mtp_case, mtp_case_failed ? "fail" : "pass", NULL, mtp_ms());
	int missing = 0;
	if (mtp_select) mtp_lines(mtp_missing_line, &missing);
	struct mtp_line e; e.n = 0; e.overflow = 0;
	mtp_cstr(&e, "{\"e\":\"done\",\"status\":");
	mtp_cstr(&e, mtp_case_failed ? "\"fail\"" : "\"pass\"");
	mtp_cstr(&e, ",\"counts\":{\"pass\":"); mtp_num(&e, (uint64_t)mtp_counts[0]);
	mtp_cstr(&e, ",\"fail\":"); mtp_num(&e, (uint64_t)mtp_counts[1]);
	mtp_cstr(&e, ",\"skip\":"); mtp_num(&e, (uint64_t)mtp_counts[2]);
	mtp_cstr(&e, "}}"); mtp_send(&e);
	close(mtp_fd);
	mtp_fd = -1;
}

/* The handshake (P2), where a runner asked for it: answers 1 when the
 * program speaks the protocol. The `done' event is then written at exit. */
MTP_FN int mtp_begin_program(void) {
	if (*(const volatile char*)mtp_marker != 'm') return 0;
	const char* protocol = getenv("MTEST_PROTOCOL");
	const char* report = getenv("MTEST_REPORT");
	if (!protocol || strcmp(protocol, "1") != 0 || !report || !*report) return 0;
	mtp_fd = open(report, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (mtp_fd < 0) return 0;
	mtp_start_ns = mtp_now_ns();
	const char* mode = getenv("MTEST_MODE");
	mtp_listing = mode && strcmp(mode, "list") == 0;

	const char* select = getenv("MTEST_SELECT");
	if (select && *select) {
		const int fd = open(select, O_RDONLY);
		if (fd >= 0) {
			size_t cap = 4096;
			mtp_select = (char*)malloc(cap + 1);
			while (mtp_select) {
				if (mtp_select_len == cap) {
					char* grown = (char*)realloc(mtp_select, cap * 2 + 1);
					if (!grown) break;
					mtp_select = grown; cap *= 2;
				}
				const ssize_t got = read(fd, mtp_select + mtp_select_len, cap - mtp_select_len);
				if (got <= 0) break;
				mtp_select_len += (size_t)got;
			}
			close(fd);
		}
	}

	/* A COPY THIS PROGRAM STARTS IS NOT A PROGRAM OF THE RUN. Several probes
	 * start themselves again; a copy that inherited the variables would write
	 * a second `program' event into this program's report. */
	unsetenv("MTEST_PROTOCOL");
	unsetenv("MTEST_REPORT");
	unsetenv("MTEST_MODE");
	unsetenv("MTEST_SELECT");

	struct mtp_line e; e.n = 0; e.overflow = 0;
	mtp_cstr(&e, "{\"e\":\"program\",\"protocol\":1,\"level\":2,\"framework\":\"" MTP_FRAMEWORK
	              "\",\"version\":\"" MTP_VERSION "\",\"isolation\":[\"process\"]}");
	mtp_send(&e);
	atexit(mtp_finish);
	return 1;
}

/* A probe: the protocol, and the program's one case. Answers 1 when the
 * program should stop at once --- the runner asked for the list of cases, or
 * selected none of this program's --- and 0 when it should go on to observe,
 * which is always so without a runner. */
MTP_FN int mtp_begin(const char* case_id) {
	size_t k = strlen(case_id);
	if (k >= sizeof mtp_case) k = sizeof mtp_case - 1;
	memcpy(mtp_case, case_id, k);
	mtp_case[k] = 0;
	if (!mtp_begin_program()) return 0;
	if (mtp_listing) { mtp_case_event("case", mtp_case, "cts"); return 1; }
	mtp_selected = mtp_wanted(mtp_case);
	if (!mtp_selected) return 1;
	mtp_record_ran(mtp_case);
	mtp_case_open = 1;
	mtp_case_event("begin", mtp_case, "cts");
	return 0;
}

/* One observation, a part of the program's case (P3.4). */
MTP_FN void mtp_observe(const char* what, int held) {
	if (!mtp_case_open) return;
	char id[1024]; size_t n = 0;
	for (size_t i = 0; mtp_case[i] && n + 1 < sizeof id; i++) id[n++] = mtp_case[i];
	if (n + 1 < sizeof id) id[n++] = '/';
	for (size_t i = 0; what && what[i] && n + 8 < sizeof id; i++) id[n++] = what[i];
	id[n] = 0;
	const uint64_t h = mtp_hash(id);
	int seen = 0;
	for (int i = 0; i < mtp_parts; i++) if (mtp_part_hash[i] == h) { seen = ++mtp_part_seen[i]; break; }
	if (seen == 0 && mtp_parts < MTP_MAX_PARTS) { mtp_part_hash[mtp_parts] = h; mtp_part_seen[mtp_parts++] = 1; }
	if (seen > 1 && n + 8 < sizeof id) {
		id[n++] = ' '; id[n++] = '#';
		char d[8]; int i = 8; int v = seen;
		while (v) { d[--i] = (char)('0' + v % 10); v /= 10; }
		for (; i < 8; i++) id[n++] = d[i];
		id[n] = 0;
	}
	struct mtp_line b; b.n = 0; b.overflow = 0;
	mtp_cstr(&b, "{\"e\":\"begin\",\"id\":"); mtp_str(&b, id, n);
	mtp_cstr(&b, ",\"t\":"); mtp_num(&b, mtp_ms());
	mtp_cstr(&b, ",\"kind\":\"cts\",\"clauses\":["); mtp_str(&b, mtp_case, strlen(mtp_case));
	mtp_cstr(&b, "],\"resources\":[\"" MTP_RESOURCE "\"]}");
	mtp_send(&b);
	if (!held) {
		mtp_case_failed = 1;
		struct mtp_line f; f.n = 0; f.overflow = 0;
		mtp_cstr(&f, "{\"e\":\"failure\",\"id\":"); mtp_str(&f, id, n);
		mtp_cstr(&f, ",\"file\":\"\",\"line\":0,\"message\":"); mtp_str(&f, what, strlen(what));
		mtp_cstr(&f, "}"); mtp_send(&f);
	}
	mtp_counts[held ? 0 : 1]++;
	mtp_end_event(id, held ? "pass" : "fail", NULL, 0);
}

/* One measured primitive: a case of kind `bench' holding its metric (P6),
 * listed in list mode and left out when the selection does not name it. */
MTP_FN void mtp_measure(const char* id, double value, const char* unit) {
	if (mtp_fd < 0) return;
	if (mtp_listing) { mtp_case_event("case", id, "bench"); return; }
	if (!mtp_wanted(id)) return;
	mtp_record_ran(id);
	mtp_case_event("begin", id, "bench");
	struct mtp_line e; e.n = 0; e.overflow = 0;
	mtp_cstr(&e, "{\"e\":\"metric\",\"id\":"); mtp_str(&e, id, strlen(id));
	mtp_cstr(&e, ",\"name\":"); mtp_str(&e, id, strlen(id));
	/* The value in thousandths, written without the C library's formatting
	 * so that a measurement of that formatting does not measure itself. */
	const uint64_t milli = value > 0 ? (uint64_t)(value * 1000.0 + 0.5) : 0;
	mtp_cstr(&e, ",\"value\":"); mtp_num(&e, milli / 1000);
	if (milli % 1000) {
		const char frac[4] = { '.', (char)('0' + milli / 100 % 10), (char)('0' + milli / 10 % 10),
		                       (char)('0' + milli % 10) };
		mtp_raw(&e, frac, 4);
	}
	mtp_cstr(&e, ",\"unit\":"); mtp_str(&e, unit, strlen(unit));
	mtp_cstr(&e, ",\"better\":\"lower\"}");
	mtp_send(&e);
	mtp_counts[0]++;
	mtp_end_event(id, "pass", NULL, 0);
}

#endif /* OKM_MTEST_PROTOCOL_H */
