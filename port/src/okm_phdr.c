/* Where the program's own headers are, on the one target format that has them.
 *
 * musl answers `dl_iterate_phdr' from the auxiliary vector, which the kernel
 * leaves on the initial stack. This port replaces `__libc_start_main' precisely
 * so that it need not read that vector --- an implementation of openkal is not
 * obliged to leave one, and two of the three formats this package builds for
 * have no such thing at all. The replacement therefore left the enquiry with
 * nothing to report.
 *
 * ⚠️ IT REPORTED THE ABSENCE AS AN ANSWER, WHICH IS THE FAILURE THIS FILE IS
 * ABOUT. `dl_iterate_phdr' returned ONE object with `dlpi_phnum == 0': a caller
 * cannot distinguish that from a program that genuinely has no segments, so it
 * concludes there is nothing to find rather than that it was not told.
 *
 * The caller that matters is the unwinder. It looks for PT_GNU_EH_FRAME among
 * the segments, finds no segments, and therefore finds no frame descriptions.
 * The measured consequence, in a program that throws:
 *
 *     libc++abi: terminating due to uncaught exception
 *
 * with no diagnostic naming the cause, and with `_Unwind_Backtrace' walking
 * ZERO frames --- while the compile, the link, and every path that does not
 * throw are green. That is the whole of the report a program gets, and it names
 * neither the unwinder nor the headers.
 *
 * The headers are still there; only the pointer to them was lost. Every linker
 * this package is built by defines `__ehdr_start' at the ELF header when the
 * headers are part of a loaded segment, which is the ordinary case and is what
 * the auxiliary vector would otherwise have reported. Taking it weakly means a
 * link that does not define it falls back to musl's answer rather than failing.
 *
 * This file is for ELF and says so. The other two formats exclude it, as they
 * already exclude musl's own dl_iterate_phdr, and for the same reason: the
 * operation names a structure those formats do not have.
 */
#include <link.h>
#include <stddef.h>

extern const ElfW(Ehdr) __ehdr_start __attribute__((weak));

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size,
                                    void *data),
                    void *data)
{
	struct dl_phdr_info info;
	const ElfW(Ehdr) *eh = &__ehdr_start;

	if (!eh) return 0;

	/* The load address is the difference between where the header is and
	 * where it said it would be. For a program linked at a fixed address the
	 * two agree and the difference is zero; for one that is position
	 * independent the first loaded segment states its own virtual address and
	 * the difference is what everything else must be adjusted by. */
	const ElfW(Phdr) *ph = (const ElfW(Phdr) *)((const char *)eh + eh->e_phoff);
	ElfW(Addr) base = (ElfW(Addr))eh;
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD) {
			base = (ElfW(Addr))eh - ph[i].p_vaddr;
			break;
		}

	info.dlpi_addr  = base;
	info.dlpi_name  = "";
	info.dlpi_phdr  = ph;
	info.dlpi_phnum = eh->e_phnum;
	/* The remaining members describe an object that was loaded and unloaded
	 * again, which cannot happen here: there is one object and it is the
	 * program. A caller reads them to decide whether what it cached is still
	 * valid, and the answer is that it always is. */
	info.dlpi_adds  = 1;
	info.dlpi_subs  = 0;
	info.dlpi_tls_modid = 0;
	info.dlpi_tls_data  = NULL;

	return callback(&info, sizeof info, data);
}
