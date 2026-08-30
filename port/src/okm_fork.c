/* fork(2), composed above openkal.space.
 *
 * ⭐ THE SPECIFICATION DESCRIBES THIS COMPOSITION AND DECLINES TO PERFORM IT,
 * WHICH IS WHY THE CODE IS HERE AND NOT BENEATH. openkal/include/openkal/space.h
 * says so in terms:
 *
 *     A library above this interface reaches `fork' by saving its own execution
 *     state before the call and restoring it in the started context. That is
 *     composition, and it belongs above this line rather than in it: the saving
 *     is done with the compiler's own facilities, differs per architecture, and
 *     is not something a kernel interface can perform on a caller's behalf.
 *
 * This port is the library that composition was described for. The saving is
 * `setjmp', whose per-architecture instruction sequences this port already
 * carries in okm_setjmp.S, and the restoring is `longjmp' in the started
 * context. Nothing beneath changes and the specification does not move.
 *
 * ⚠️ AN EARLIER READING OF openkal-linux#13 CONCLUDED THAT THE SPECIFICATION HAD
 * DELIBERATELY DECLINED fork, on the ground that duplicating an address space
 * AND ITS EXECUTION STATE cannot be required of every environment. Half of that
 * is right and the conclusion drawn from it was wrong: clause 7.1 declines the
 * PAIR, and `openkal.space' supplies the first half by itself. What was missing
 * was never the atom. It was this file, and a `case SYS_clone' in the
 * dispatcher.
 *
 * --- why the copied stack is the one the child returns on -------------------
 *
 * `kal_space_start' calls `entry' in the copy. The copy contains this
 * function's own frame at the same address it has here, because that is what
 * copying an address space means --- so the context `setjmp' recorded is valid
 * in the child, and `longjmp' unwinds to a frame that is OLDER than the one
 * `entry' is standing on. That direction is the one longjmp is defined for.
 *
 * An implementation that gives the started context a stack of its own instead
 * reaches the same place by the same route: `entry' stands on the stack this
 * file supplies, and `longjmp' leaves it for the copied one. Both are correct
 * and a caller cannot observe which happened, which is what the header says
 * about the stack argument.
 */
#define _GNU_SOURCE
#include "okm.h"
#include "okm_opt.h"

#include <openkal/space.h>

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>

/* okm_context.c --- where the state belonging to one execution context is kept,
 * keyed on the identity openkal gives that context. */
uintptr_t __okm_get_tp(void);
void      __okm_set_tp(uintptr_t);
void*     __okm_get_self(void);
void      __okm_set_self(void*);

/* Weak, by the rule okm_net.c states: `openkal.space' is optional --- no
 * bare-metal backend has a second address space to make --- and a strong
 * reference would make a program that never calls `fork' fail to link. */
extern __typeof(kal_space_start) kal_space_start __attribute__((__weak__));

int  __okm_child_record(struct kal_process h);   /* okm_syscall.c */
void __okm_forget_children(void);               /* okm_syscall.c */
int  __okm_child_reserve(int* pid_out);         /* okm_syscall.c --- lock held */
void __okm_child_commit(int slot, struct kal_process h);
void __okm_child_release(int slot);
void __okm_set_self_pid(int pid);

/* The context the started child resumes into.
 *
 * ⚠️ ONE, AND THE PORT'S OWN LOCK AROUND IT. Two contexts forking at once would
 * otherwise record over each other and the second child would resume into the
 * first one's frame. This port's table lock is the one taken rather than a lock
 * of this file's, and holding it does a second job worth having: the copy is
 * taken while no other context is part-way through a change to the descriptor
 * table, so the child begins with a table that is whole.
 *
 * ⚠️ THE CHILD RELEASES ITS COPY OF THE LOCK. It was taken before the copy, so
 * the copy holds it too, and a child that did not release it would stop at the
 * first descriptor it touched. */
static jmp_buf g_resume;

/* ⚠️⚠️ WHAT THE COPY CARRIES AND WHAT IT DOES NOT: THE IDENTITY IS NOT CARRIED.
 *
 * okm_context.c keeps this library's per-context state --- its error value, its
 * locale, its thread record --- in a table keyed on `kal_task_current()'. The
 * specification says that identity is "unique among contexts running at the same
 * moment and may be reused after one ends". It says NOTHING about a copy of the
 * address space, and two implementations answer differently:
 *
 *     openkal-linux   caches `gettid' in a thread-local, so the COPY of the
 *                     cache answers the parent's value and the lookup succeeds
 *     openkal-macos   asks `thread_selfid' every time, so the started context
 *                     is a NEW thread of a NEW process and answers a value the
 *                     table has never seen
 *
 * ⇒ The second is not a defect. It is the honest answer to the question the
 * interface asks, and the assumption that a copy keeps its identity was this
 * port's.
 *
 * ⭐ MEASURED, AND THE PORT'S OWN DIAGNOSTIC NAMED IT. On the macOS row the
 * copy stopped with
 *
 *     openkal-musl: this execution context has no per-context state --- the
 *     implementation's kal_task_current did not answer the same value here as
 *     it did when the context started
 *
 * which is the message `__okm_get_tp' has carried since it was written, for
 * exactly this condition. Without it the report would have been a copy that
 * ended on a signal, four layers from the cause.
 *
 * ⇒ The started context REBINDS ITS SLOT before anything reads per-context
 * state. The values are read here, in the original, and are globals because a
 * local written between `setjmp' and `longjmp' is indeterminate in the resumed
 * context. */
static volatile uintptr_t g_carried_tp;
static void* volatile     g_carried_self;

/* ⭐ AND THE IDENTIFIER, WHICH IS CARRIED FOR THE SAME REASON AND BY THE SAME
 * MEANS. `getpid' answered the constant 1 in every context, so a copy reported
 * the identifier of the image it was copied from: two contexts, one answer, and
 * no way for the copy to name itself. The number the copy should give is the one
 * the parent's `fork' returns, so it has to exist BEFORE the copy is taken ---
 * which is why the table entry is now reserved above rather than recorded below. */
static volatile int       g_carried_pid;

/* A stack for the entry function, used only by an implementation that honours
 * the argument. All it holds is one call to `longjmp'. */
static char g_entry_stack[8192] __attribute__((aligned(16)));

static void child_entry(void* arg)
{
	(void)arg;
	longjmp(g_resume, 1);
}

/* Returns the child's identifier to the caller and zero to the child, which is
 * what `fork' means and what no C function can do by returning once. */
syscall_arg_t __okm_fork(void)
{
	if (!kal_space_start) return -ENOSYS;

	okm_lock();

	g_carried_tp   = __okm_get_tp();
	g_carried_self = __okm_get_self();

	/* ⚠️ THE ENTRY IS TAKEN BEFORE THE CONTEXT EXISTS, so that the identifier
	 * settled here is the one the copy reads out of its own copy of this global.
	 * The lock this function already holds is the table's, which is what makes
	 * reserving here safe and what makes a second acquisition wrong.
	 *
	 * ⚠️ `volatile', although both are written BEFORE the `setjmp' below and are
	 * read only on the path that does not resume through it. That is enough to
	 * be correct and is not enough to be obviously correct: this file's rule is
	 * that a local live across that call says so, and a reader checking the rule
	 * should not have to reconstruct which path reads which. */
	int scratch = 0;
	const volatile int slot = __okm_child_reserve(&scratch);
	if (slot < 0) { okm_unlock(); return -EAGAIN; }
	const volatile int reserved_pid = scratch;
	g_carried_pid = reserved_pid;

	/* ⚠️ NOTHING BELOW THIS LINE MAY READ A LOCAL VARIABLE THAT WAS WRITTEN
	 * AFTER IT. A variable modified between `setjmp' and `longjmp' and not
	 * declared volatile is indeterminate in the resumed context; the child path
	 * therefore reads nothing but the two globals above and returns a constant. */
	if (setjmp(g_resume) != 0) {
		/* ⚠️ THIS IS THE FIRST THING THE COPY DOES, AND THE ORDER IS THE POINT.
		 * `okm_unlock' is an atomic store and touches no per-context state;
		 * everything after it does. */
		__okm_set_tp(g_carried_tp);
		__okm_set_self(g_carried_self);
		/* The entries in the child table name programs the ORIGINAL started,
		 * and POSIX is explicit that a duplicate has no children. okm_syscall.c
		 * records what keeping them would cost. */
		__okm_forget_children();
		/* ⭐ AND THE COPY NAMES ITSELF. Read from this file's own global, which
		 * the copy carries because it was written before the copy was taken;
		 * `__okm_forget_children' above cleared the TABLE and not this. */
		__okm_set_self_pid((int)g_carried_pid);
		okm_unlock();
		return 0;                     /* the child */
	}

	struct kal_process child;
	const int e = kal_space_start(child_entry, 0,
	                              g_entry_stack + sizeof g_entry_stack, &child);
	/* ⚠️ THE ENTRY WAS TAKEN BEFORE THE CONTEXT WAS STARTED, so a start that
	 * failed has to give it back --- otherwise a program whose every `fork'
	 * fails would exhaust the table and begin reporting EAGAIN for a reason
	 * that has nothing to do with how many children it has. */
	if (e != kal_ok) { __okm_child_release(slot); okm_unlock(); return -okm_errno(e); }
	__okm_child_commit(slot, child);
	okm_unlock();
	return (syscall_arg_t)reserved_pid;
}
