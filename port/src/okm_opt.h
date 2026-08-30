/* The seam between the C library and the openkal interfaces that need not be
 * there.
 *
 * ⚠️ WHY A SEAM RATHER THAN A GUARD AT EACH CALL.
 *
 * `openkal.fs' and `openkal.process' are called from about forty places in
 * okm_syscall.c, spread through a switch on the system call number. Guarding
 * each would put one decision in forty places, and a forty-first added later
 * would be the one that was missed --- and missed silently, because the way it
 * shows is a link failure on a target nobody was building at the time.
 *
 * So the decision is made once, here, and every caller names `okm_fs_*' and
 * `okm_process_*'. Where the interface is present these are the interface, with
 * no code generated for the indirection. Where it is not, they are the answer.
 *
 * ⭐ AND THE ANSWER IS AN ERROR, WHICH IS ALLOWED HERE AND FORBIDDEN BELOW.
 *
 * openkal clause 6.2 says an operation that is present and always fails is a
 * defect, and that the remedy is that its absence be expressed by its absence.
 * That rule binds an openkal IMPLEMENTATION. This file is not one: nothing here
 * defines a `kal_*' symbol, and a program linked against this library and a
 * core-only implementation still gets a link failure if it calls `kal_fs_open'
 * itself.
 *
 * What this file does is answer for the C LIBRARY, whose surface is POSIX ---
 * and POSIX has an answer for this. `open' may report ENOSYS. Every caller of
 * `open' already handles a failure. So the absence that openkal expresses by
 * absence becomes, one layer up, an absence expressed by a defined error, and
 * the program that never opens a file never notices either.
 *
 * ⚠️ THE ONE WAY THIS COULD GO WRONG IS NOT PRESENT: nothing below reports
 * SUCCESS having done nothing. Every operation that could have done something
 * fails. The exceptions are marked where they appear and each is an operation
 * that is COMPLETE when there is nothing to do --- releasing a handle that
 * cannot have been obtained, joining a context that was never started, waking
 * the zero waiters there are.
 */
#ifndef OKM_OPT_H
#define OKM_OPT_H

#include "okm.h"

#if OKM_HAS_FS

#define okm_fs_preopen_count kal_fs_preopen_count
#define okm_fs_preopen       kal_fs_preopen
#define okm_fs_open_dir      kal_fs_open_dir
#define okm_fs_open          kal_fs_open
#define okm_fs_props         kal_fs_props
#define okm_fs_max_name      kal_fs_max_name
#define okm_fs_close_dir     kal_fs_close_dir
#define okm_fs_close_file    kal_fs_close_file
#define okm_fs_stream        kal_fs_stream
#define okm_fs_seek          kal_fs_seek
#define okm_fs_truncate      kal_fs_truncate
#define okm_fs_info          kal_fs_info
#define okm_fs_mkdir         kal_fs_mkdir
#define okm_fs_remove        kal_fs_remove
#define okm_fs_rename        kal_fs_rename
#define okm_fs_file_info     kal_fs_file_info
#define okm_fs_set_modified  kal_fs_set_modified
#define okm_fs_set_modified_at kal_fs_set_modified_at
#define okm_fs_lock          kal_fs_lock
#define okm_fs_unlock        kal_fs_unlock
#define okm_fs_capacity      kal_fs_capacity
#define okm_fs_list_begin    kal_fs_list_begin
#define okm_fs_list_next     kal_fs_list_next

/* ⚠️ WEAK, BECAUSE A VOLUME MAY HAVE NO SUCH NODES AND AN IMPLEMENTATION MAY
 * NOT BE ABLE TO MAKE ONE. These are operations of `openkal.fs' and are present
 * wherever it is, so the weakness is not about the interface --- it is about a
 * backend built before they existed. `kal_fs_props' answers, per directory,
 * whether they can be used, and this port asks before it calls. */
extern __typeof(kal_fs_link_create) kal_fs_link_create __attribute__((__weak__));
extern __typeof(kal_fs_link_read)   kal_fs_link_read   __attribute__((__weak__));

/* ⚠️⚠️ AND THEY GO THROUGH THE SEAM LIKE EVERYTHING ELSE, WHICH THEY DID NOT.
 *
 * The two call sites named `kal_fs_link_*' directly and tested the weak symbol
 * themselves. That is correct where `openkal.fs' is present and is not a
 * declaration at all where it is absent --- the weak declarations above are
 * inside this branch, so in the OKM_HAS_FS == 0 configuration the names came
 * from <openkal/fs.h> STRONG, and a backend with no filesystem failed to link:
 *
 *     ld.lld: error: undefined symbol: kal_fs_link_read
 *     >>> referenced by okm_syscall.c:340 ... (do_readlink)
 *
 * ⭐ WHICH IS THE FAILURE THIS FILE'S OWN OPENING COMMENT PREDICTS, IN THE
 * WORDS IT PREDICTS IT IN: "a forty-first added later would be the one that was
 * missed --- and missed silently, because the way it shows is a link failure on
 * a target nobody was building at the time". It was found by openkal-opensbi's
 * bare-metal row, which is the only row that builds this configuration.
 *
 * So the null test lives here, once, and the callers name `okm_fs_link_*'. */
static inline int okm_fs_link_create(struct kal_dir base, const char* name,
                                     kal_uintptr len, const char* target,
                                     kal_uintptr target_len, kal_uintptr flags) {
	if (!kal_fs_link_create) return kal_err_not_supported;
	return kal_fs_link_create(base, name, len, target, target_len, flags);
}
static inline kal_intptr okm_fs_link_read(struct kal_dir base, const char* name,
                                          kal_uintptr len, char* out,
                                          kal_uintptr cap) {
	if (!kal_fs_link_read) return -kal_err_not_supported;
	return kal_fs_link_read(base, name, len, out, cap);
}

#else

/* `static inline' and not a macro, so that an argument is still type-checked
 * and a change to an openkal signature is still a compile error in the
 * configuration that does not use it. A macro expanding to a constant would
 * make this file the one place where a signature change goes unnoticed. */
static inline kal_uintptr okm_fs_preopen_count(void) { return 0; }
static inline int okm_fs_preopen(kal_uintptr, struct kal_dir*, char*,
                                 kal_uintptr, kal_uintptr*) { return kal_err_not_supported; }
static inline kal_uintptr okm_fs_props(struct kal_dir) { return 0; }
static inline kal_uintptr okm_fs_max_name(void) { return 0; }
static inline int okm_fs_open_dir(struct kal_dir, const char*, kal_uintptr,
                                  struct kal_dir*) { return kal_err_not_supported; }
static inline int okm_fs_open(struct kal_dir, const char*, kal_uintptr, kal_uintptr,
                              struct kal_file*) { return kal_err_not_supported; }
/* Releasing a handle that cannot have been obtained. There is nothing to
 * release and nothing to report --- the openkal operations are void too. */
static inline void okm_fs_close_dir(struct kal_dir) {}
static inline void okm_fs_close_file(struct kal_file) {}
/* ⚠️ Zero, which is not a valid stream, and it is only ever reached with a file
 * handle no operation above can have produced. A caller that reaches here has
 * already ignored a failure, and zero is what it then passes to
 * `kal_stream_write', which refuses it. */
static inline struct kal_stream okm_fs_stream(struct kal_file) {
	struct kal_stream s = { 0 };
	return s;
}
static inline int okm_fs_seek(struct kal_file, kal_i64, int,
                              kal_u64*) { return kal_err_not_supported; }
static inline int okm_fs_truncate(struct kal_file, kal_u64) { return kal_err_not_supported; }
static inline int okm_fs_info(struct kal_dir, const char*, kal_uintptr,
                              kal_uintptr, kal_u32,
                              struct kal_node_info*) { return kal_err_not_supported; }
static inline int okm_fs_mkdir(struct kal_dir, const char*,
                               kal_uintptr) { return kal_err_not_supported; }
static inline int okm_fs_link_create(struct kal_dir, const char*, kal_uintptr,
                                     const char*, kal_uintptr,
                                     kal_uintptr) { return kal_err_not_supported; }
static inline kal_intptr okm_fs_link_read(struct kal_dir, const char*, kal_uintptr,
                                          char*, kal_uintptr) { return -kal_err_not_supported; }
static inline int okm_fs_remove(struct kal_dir, const char*,
                                kal_uintptr) { return kal_err_not_supported; }
static inline int okm_fs_rename(struct kal_dir, const char*, kal_uintptr,
                                struct kal_dir, const char*,
                                kal_uintptr) { return kal_err_not_supported; }
static inline int okm_fs_file_info(struct kal_file, kal_u32,
                                   struct kal_node_info*) { return kal_err_not_supported; }
static inline int okm_fs_set_modified(struct kal_file, kal_u64) { return kal_err_not_supported; }
static inline int okm_fs_set_modified_at(struct kal_dir, const char*, kal_uintptr,
                                         kal_u64) { return kal_err_not_supported; }
static inline int okm_fs_lock(struct kal_file, kal_u64, kal_u64,
                              kal_uintptr) { return kal_err_not_supported; }
static inline int okm_fs_unlock(struct kal_file, kal_u64,
                                kal_u64) { return kal_err_not_supported; }
static inline int okm_fs_capacity(struct kal_dir, kal_u64*,
                                  kal_u64*) { return kal_err_not_supported; }
static inline int okm_fs_list_begin(struct kal_dir, kal_uintptr*) { return kal_err_not_supported; }
static inline int okm_fs_list_next(struct kal_dir, kal_uintptr*, char*, kal_uintptr,
                                   kal_uintptr*, int*) { return kal_err_not_supported; }

#endif  /* OKM_HAS_FS */

#if OKM_HAS_PROCESS

#define okm_process_spawn     kal_process_spawn
#define okm_process_spawn_bound kal_process_spawn_bound
#define okm_process_wait      kal_process_wait
#define okm_process_terminate kal_process_terminate
#define okm_process_close     kal_process_close

/* ⭐⭐ THIS WAS THE ONE WEAK REFERENCE THAT NEEDED A DIFFERENT SPELLING, AND
 * openkal 0.9 REMOVED THE REASON.
 *
 * `kal_process_props' was an OBJECT. Every other weak reference in this port
 * names a function, so the established form is `if (kal_net_connect) …' --- the
 * name decays to its address and the test is upon that address. Written the
 * same way for an object, the test READS the object, and where the definition
 * is absent the object is at address zero: the test intended to prevent a null
 * dereference WAS one. The address had to be taken, and this file existed in
 * part to say so.
 *
 * Version 0.9 makes every report an operation. The two kinds of symbol are one
 * kind, the special case is gone, and no caller has to remember which it is
 * holding. */
extern __typeof(kal_process_props) kal_process_props __attribute__((__weak__));

static inline kal_uintptr okm_process_props(void)
{
	return kal_process_props ? kal_process_props() : 0;
}

#else

/* Nothing is provided, so nothing is claimed. */
static inline kal_uintptr okm_process_props(void) { return 0; }

static inline int okm_process_spawn(struct kal_dir, const char*, kal_uintptr,
                                    const char**, const kal_uintptr*, kal_uintptr,
                                    const char**, const kal_uintptr*, kal_uintptr,
                                    const struct kal_spawn_streams*,
                                    struct kal_process*) { return kal_err_not_supported; }
static inline int okm_process_spawn_bound(struct kal_dir, const char*, kal_uintptr,
                                    const char**, const kal_uintptr*, kal_uintptr,
                                    const char**, const kal_uintptr*, kal_uintptr,
                                    const struct kal_spawn_streams*,
                                    struct kal_process*) { return kal_err_not_supported; }
static inline int okm_process_wait(struct kal_process, int*,
                                   int*) { return kal_err_not_supported; }
static inline int okm_process_terminate(struct kal_process) { return kal_err_not_supported; }
static inline void okm_process_close(struct kal_process) {}

#endif  /* OKM_HAS_PROCESS */

#if OKM_HAS_TASK

#define okm_task_start kal_task_start
#define okm_task_join  kal_task_join
#define okm_task_yield kal_task_yield
#define okm_task_wait  kal_task_wait
#define okm_task_wake  kal_task_wake

/* ⭐ WEAK, BECAUSE openkal 0.10 ADDED IT AND A BACKEND MAY NOT HAVE FOLLOWED.
 * Every route in this port that reaches an optional operation tests the
 * reference before calling; this one is reached from `sched_getaffinity', which
 * a program asks once at startup and must not fault in. */
extern __typeof(kal_task_parallelism) kal_task_parallelism __attribute__((__weak__));
#define okm_task_parallelism kal_task_parallelism

/* The identity of the execution context in progress. */
#define OKM_CONTEXT_ID() kal_task_current()

#else

/* Starting one. `__clone' turns any failure into EAGAIN, which is the value
 * `pthread_create' is specified to report when the resources for a context are
 * unavailable --- and it has no value for "this system has no contexts". A
 * program written against POSIX handles EAGAIN; a value POSIX does not list for
 * the call entitles it to do anything. So the seam reports the ordinary
 * failure and the caller's existing path produces the right errno. */
static inline int okm_task_start(void (*)(void*), void*,
                                 struct kal_task*) { return kal_err_not_supported; }
/* Joining one that cannot have been started. The list of contexts to join is
 * empty because nothing is ever added to it. */
static inline int okm_task_join(struct kal_task) { return kal_ok; }
/* Yielding with nothing to yield to. Returning is the whole of it, and it is
 * accurate rather than a stand-in: the caller asked to let something else run,
 * everything else has already run, and control comes back. */
static inline void okm_task_yield(void) {}
/* ⚠️ THE ONE ANSWER HERE THAT WOULD HANG RATHER THAN FAIL IF IT WERE WRONG.
 *
 * musl waits like this: `while (*addr == val) futex(WAIT)'. With one execution
 * context nothing can change *addr while this call is in progress, so a caller
 * that reaches here is already stuck --- and `kal_err_again', which means "the
 * value did not match, look again", would send it round that loop forever and
 * SILENTLY. The unsupported answer becomes ENOSYS and the caller is told the
 * wait did not happen. */
static inline int okm_task_wait(const kal_u32*, kal_u32,
                                kal_u64) { return kal_err_not_supported; }
/* ⚠️ SUCCESS, AND IT IS THE THIRD AND LAST OPERATION HERE THAT REPORTS ANY.
 *
 * Waking every waiter is complete when there are none, and zero is the count
 * rather than a refusal --- a wake with no waiter is an ordinary outcome on
 * every system, and musl's callers already ignore the number. Reporting a
 * failure instead would make an operation that succeeded look like one that
 * did not. */
static inline int okm_task_wake(const kal_u32*, kal_uintptr,
                                kal_uintptr* woken) { if (woken) *woken = 0; return kal_ok; }

/* One context, and its identity is a constant. Not a placeholder: it is the
 * accurate answer for a machine that has one, and it keeps okm_context.c's
 * per-context table working unchanged rather than growing a second shape. */
#define OKM_CONTEXT_ID() ((kal_uintptr)1)

/* Where contexts are withheld there is exactly one, and this says so rather
 * than refusing: a program sizing itself against one context on a machine that
 * has one is sizing itself correctly. */
static inline kal_uintptr okm_task_parallelism_absent(void) { return 1; }
#define okm_task_parallelism okm_task_parallelism_absent

#endif  /* OKM_HAS_TASK */

#endif /* OKM_OPT_H */
