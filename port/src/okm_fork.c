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

/* Weak, by the rule okm_net.c states: `openkal.space' is optional --- no
 * bare-metal backend has a second address space to make --- and a strong
 * reference would make a program that never calls `fork' fail to link. */
extern __typeof(kal_space_start) kal_space_start __attribute__((__weak__));

int __okm_child_record(struct kal_process h);   /* okm_syscall.c */

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

	/* ⚠️ NOTHING BELOW THIS LINE MAY READ A LOCAL VARIABLE THAT WAS WRITTEN
	 * AFTER IT. A variable modified between `setjmp' and `longjmp' and not
	 * declared volatile is indeterminate in the resumed context; the child path
	 * therefore reads nothing at all and returns a constant. */
	if (setjmp(g_resume) != 0) {
		okm_unlock();
		return 0;                     /* the child */
	}

	struct kal_process child;
	const int e = kal_space_start(child_entry, 0,
	                              g_entry_stack + sizeof g_entry_stack, &child);
	/* ⚠️ RELEASED BEFORE THE CHILD IS RECORDED, because the recording takes the
	 * same lock and this one does not nest. The child was copied above and
	 * holds its own copy of what was released here. */
	okm_unlock();
	if (e != kal_ok) return -okm_errno(e);

	const int pid = __okm_child_record(child);
	if (pid < 0) { okm_process_close(child); return -EAGAIN; }
	return (syscall_arg_t)pid;
}
