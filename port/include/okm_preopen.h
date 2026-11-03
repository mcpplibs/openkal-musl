/* The directories a program holds, attenuated at the program's own request
 * (openkal-musl 0.27.0; openkal 0.23, kal_fs_dir_attenuate).
 *
 * THIS LIBRARY RESOLVES A NAME AGAINST THE DIRECTORIES THE PROGRAM WAS GIVEN
 * (kal_fs_preopen): an absolute name against the one whose name is its
 * longest prefix, a relative name against the directory the program is in.
 * A program that asks openkal for a handle with less --- read-only, unchanging,
 * or recorded --- holds a new handle, and the names it passes to `open' still
 * reach the old one. This replaces the one this library resolves against.
 *
 * `name' is a held directory's name as kal_fs_preopen states it. `flags' are
 * the KAL_DIR_* positions of <openkal/fs.h>. With KAL_DIR_RECORDED, `record'
 * names the file the implementation appends each name reached beneath to; it
 * is opened for appending (created if absent, not truncated, so that the
 * program may write a heading first), before the directory is attenuated, and
 * held open until the program ends, as the record requires. The directory the
 * program is in, where it is the held one, moves to the new handle. The table
 * of held directories is read without a lock: a program calls this before its
 * other contexts reach the file system, as a worker does at its start.
 *
 * 0, or -1 and errno: ENOENT for a name no held directory has, ENOTSUP where
 * the volume does not attenuate (KAL_FS_PROP_ATTENUATE), EINVAL for a flag
 * openkal does not know, and what `open' reports of `record'. */
#ifndef OKM_PREOPEN_H
#define OKM_PREOPEN_H

#ifdef __cplusplus
extern "C" {
#endif

int okm_preopen_attenuate(const char* name, unsigned long flags, const char* record);

#ifdef __cplusplus
}
#endif

#endif
