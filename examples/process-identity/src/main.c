/* A program's identifier, and its claim on the processors (0.24.0).
 *
 * THE IDENTIFIER IS THE ENVIRONMENT'S. `getpid' answered 1 in every program,
 * so a number one program wrote where another would read it --- a lock file's
 * owner --- named the reader. It is now the identifier the environment gives
 * the program (kal_process_info), and another program is asked about by it
 * (kal_process_open_id): `kill(pid, 0)' and `getsid' say whether it runs.
 *
 * THE OBSERVATION NEEDS A PROGRAM THIS ONE DID NOT START, and openkal states no
 * parent, so the probe makes one: it starts a sleeper A, then a program B that
 * asks about A from outside, ends A, and starts a program C that asks again.
 *
 * THE CLAIM IS openkal 0.19's kal_task_set_priority, reached through the
 * scheduling policies and `setpriority' in a thread of its own. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
static int failures;
static void check(int ok, const char* what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) failures++;
}

/* --ask <pid> alive|gone: what this program, which did not start <pid>, is told. */
static int ask(const char* pid_text, const char* expect)
{
	const pid_t pid = (pid_t)atol(pid_text);
	errno = 0;
	const int k = kill(pid, 0);
	const int ke = errno;
	errno = 0;
	const pid_t s = getsid(pid);
	const int se = errno;
	if (strcmp(expect, "alive") == 0) return k == 0 && s == pid ? 0 : 10 + (k != 0) + 2 * (s != pid);
	return k == -1 && ke == ESRCH && s == -1 && se == ESRCH ? 0 : 20;
}

static int g_claims[4];
static void* lowers(void* p)
{
	(void)p;
	struct sched_param none = { 0 };
	g_claims[0] = pthread_setschedparam(pthread_self(), SCHED_IDLE, &none);
	g_claims[1] = pthread_setschedparam(pthread_self(), SCHED_FIFO, &none);
	g_claims[2] = setpriority(PRIO_PROCESS, 0, 10) == 0 ? 0 : errno;
	g_claims[3] = setpriority(PRIO_PROCESS, 0, -5) == 0 ? 0 : errno;
	return NULL;
}

static pid_t start(const char* self, char* const* av)
{
	pid_t pid = 0;
	return posix_spawn(&pid, self, NULL, NULL, av, environ) == 0 ? pid : -1;
}

static int status_of(pid_t pid)
{
	int st = 0;
	if (waitpid(pid, &st, 0) != pid) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

int main(int argc, char** argv)
{
	if (argc == 4 && strcmp(argv[1], "--ask") == 0) return ask(argv[2], argv[3]);
	if (argc == 2 && strcmp(argv[1], "--sleep") == 0) { sleep(30); return 0; }

	const pid_t me = getpid();
	printf("getpid = %ld\n", (long)me);
	check(me > 1, "a program's identifier is the environment's, not 1");
	check(getpid() == me, "it is the same answer each time");
	check(kill(me, 0) == 0, "kill(getpid(), 0) finds this program");
	check(getsid(0) == me && getsid(me) == me, "getsid of this program is its identifier");

	char* sleep_av[] = { argv[0], "--sleep", NULL };
	const pid_t a = start(argv[0], sleep_av);
	check(a > 1, "a program is started, and its identifier is the environment's");
	if (a > 1) {
		check(kill(a, 0) == 0, "kill(child, 0) finds a running child");
		char text[24];
		snprintf(text, sizeof text, "%ld", (long)a);
		char* b_av[] = { argv[0], "--ask", text, "alive", NULL };
		const pid_t b = start(argv[0], b_av);
		check(b > 1 && status_of(b) == 0, "another program finds it running: kill(pid, 0) is 0, getsid(pid) is pid");
		check(kill(a, SIGKILL) == 0, "the child is ended");
		status_of(a);
		char* c_av[] = { argv[0], "--ask", text, "gone", NULL };
		const pid_t c = start(argv[0], c_av);
		check(c > 1 && status_of(c) == 0, "and once it has ended, finds it gone: ESRCH from both");
	}

	pthread_t t;
	if (pthread_create(&t, NULL, lowers, NULL) == 0) {
		pthread_join(t, NULL);
		printf("claims: SCHED_IDLE %d, SCHED_FIFO %d, nice 10 %d, nice -5 %d\n", g_claims[0], g_claims[1], g_claims[2], g_claims[3]);
		check(g_claims[0] == 0 || g_claims[0] == ENOSYS, "SCHED_IDLE lowers a thread's claim (ENOSYS where the environment has none)");
		check(g_claims[1] == EPERM, "a real-time policy is refused, EPERM");
		/* After SCHED_IDLE a nice value is a raise, which an unprivileged thread may
		 * be refused (EACCES, as POSIX words it for `setpriority'). */
		check(g_claims[2] == 0 || g_claims[2] == EACCES || g_claims[2] == ENOSYS, "a positive nice value is a lower claim, or a raise refused");
		check(g_claims[3] == EACCES, "a negative nice value is refused, EACCES");
	}
	printf("-- failures: %d --\n", failures);
	return failures != 0;
}
