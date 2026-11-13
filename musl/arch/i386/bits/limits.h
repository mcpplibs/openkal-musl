/* openkal-musl: no PAGESIZE. The page is the environment's, asked when the
 * program starts (`libc.page_size', port/src/okm_start.c), as on aarch64: a
 * constant 4096 made `sysconf(_SC_PAGESIZE)' answer 4096 where mappings are
 * made and released in 65536 (Windows), so a release of one page was refused. */
