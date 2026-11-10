/* FreeBSD's `elf_aux_info' (<sys/auxv.h>, FreeBSD 12) over musl's auxiliary
 * vector, on the FreeBSD rows (port/include/sys/auxv.h says why). It answers as
 * FreeBSD's does: 0 and the entry's value in `buf' --- a word or an int, by
 * `buflen' --- ENOENT for an entry the vector does not hold, and EINVAL for an
 * argument it cannot answer. errno is left as it was. */
#if defined(__FreeBSD__)
#include <errno.h>
#include <string.h>
#include <sys/auxv.h>

int elf_aux_info(int aux, void *buf, int buflen)
{
	if (aux < 0 || !buf || buflen <= 0) return EINVAL;
	const int saved = errno;
	errno = 0;
	const unsigned long v = getauxval((unsigned long)aux);
	const int missing = v == 0 && errno == ENOENT;
	errno = saved;
	if (missing) return ENOENT;
	if (buflen == (int)sizeof(unsigned long)) { memcpy(buf, &v, sizeof v); return 0; }
	if (buflen == (int)sizeof(int)) { const int i = (int)v; memcpy(buf, &i, sizeof i); return 0; }
	return EINVAL;
}
#else
/* An empty translation unit is not ISO C. */
typedef int okm_elf_aux_info_is_freebsds;
#endif
