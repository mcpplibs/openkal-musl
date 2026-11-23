/* `__riscv_flush_icache' (and its public name `riscv_flush_icache'), on riscv64
 * Linux: the call the compiler makes for `__builtin___clear_cache' there, which
 * the C library defines. musl's own, src/linux/cache.c, is excluded for its vDSO
 * use (mcpp.toml says why), and a program that performs the maintenance itself
 * --- the specification's conformance suite does --- did not link without it
 * (measured: openkal-platform 0.32.1, `riscv64-linux-musl').
 *
 * Above openkal, instructions run only from memory openkal.exec published
 * (okm_map.c refuses PROT_EXEC), and the implementation performs the
 * maintenance at publication where its environment offers it as a call of its
 * own (openkal-linux 0.27.1 calls the kernel's `riscv_flush_icache'). A range a
 * program wrote is not executable before it is published, so nothing is left
 * for this call to do, and it answers success, as the kernel's does. */
#if defined(__riscv) && defined(__linux__)
#include <features.h>

int __riscv_flush_icache(void *start, void *end, unsigned long int flags)
{
	(void)start;
	(void)end;
	(void)flags;
	return 0;
}
weak_alias(__riscv_flush_icache, riscv_flush_icache);
#else
/* An empty translation unit is not ISO C. */
typedef int okm_cache_is_riscv_linuxs;
#endif
