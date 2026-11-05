/* The seam runs above the host C library. Load its headers before the port's
 * private syscall declarations: Darwin also has a syscall_arg_t, with a
 * different type. Only the port's private name is renamed in this test. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#define syscall_arg_t okm_realpath_test_syscall_arg_t
#include "okm.h"
#undef syscall_arg_t
