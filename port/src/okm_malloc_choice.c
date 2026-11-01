/* Exactly one allocator (0.26.0).
 *
 * `malloc-mimalloc' and `malloc-mallocng' each define `malloc' and the names
 * musl calls its allocator by. Both selected, the link would report every name
 * twice; neither, it would report them missing, far from the reason. So the
 * build stops here first, saying which spelling selects one. */

#if defined(MCPP_FEATURE_MALLOC_MIMALLOC) && defined(MCPP_FEATURE_MALLOC_MALLOCNG)
#error "openkal-musl: the features `malloc-mimalloc' and `malloc-mallocng' are both selected, and a program has one allocator. `malloc-mimalloc' is a default feature: to select musl's own, depend on openkal-musl with `default-features = false' and list the features to keep, `malloc-mallocng' among them."
#elif !defined(MCPP_FEATURE_MALLOC_MIMALLOC) && !defined(MCPP_FEATURE_MALLOC_MALLOCNG)
#error "openkal-musl: no allocator is selected. Keep the default features, or name `malloc-mimalloc' or `malloc-mallocng' among the features of the dependency on openkal-musl."
#endif

/* ISO C requires a translation unit to declare something. */
typedef int okm_malloc_choice;
