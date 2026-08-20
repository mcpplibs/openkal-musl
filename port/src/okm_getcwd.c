/* Reporting the working directory.
 *
 * musl's own version of this function is replaced, and the reason is one line
 * of it: having asked the kernel for the name, it refuses an answer that does
 * not begin with a separator.
 *
 *     if (ret == 0 || buf[0] != '/') { errno = ENOENT; return 0; }
 *
 * That is correct wherever musl runs, because every system musl was written for
 * spells an absolute name that way. It is not correct here. openkal does not
 * say how an environment spells a global name --- it says only that a supplied
 * directory has one and that the names are distinct --- and one of the
 * environments this library runs above writes a volume first.
 *
 * The check is the whole of the difference. Everything else is musl's, and the
 * behaviour a program observes --- a null answer for a buffer of no size, a
 * newly obtained buffer when none was given --- is unchanged.
 *
 * A program that goes on to take the name apart with the rules of one system
 * will still be taking apart a name written in another's. That is a boundary of
 * the port rather than of this function, and the README records it.
 */
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include "syscall.h"

char *getcwd(char *buf, size_t size)
{
	char tmp[buf ? 1 : PATH_MAX];
	if (!buf) {
		buf = tmp;
		size = sizeof tmp;
	} else if (!size) {
		errno = EINVAL;
		return 0;
	}
	const syscall_arg_t ret = syscall(SYS_getcwd, buf, size);
	if (ret < 0) return 0;
	if (ret == 0) { errno = ENOENT; return 0; }
	return buf == tmp ? strdup(buf) : buf;
}
