// `restrict' is a C++ program's name (0.22.7).
//
// In C it is a keyword; in C++ it is an identifier, and musl's public headers
// do not need it as a macro --- they write `__restrict'. This unit includes
// <stdio.h> and then uses the word as a variable, a member and a parameter. If
// the library defines it again, `int restrict = 2;' becomes
// `int __restrict = 2;' and this file does not compile: the compile is the
// criterion, as for the three names main.c declares. Measured where it
// mattered: clang's sources pass `restrict' to a macro, and building clangd over
// this library named the wrong token while the definition stood.
#include <stdio.h>

namespace {
struct holder { int restrict; };
int twice(int restrict) { return restrict * 2; }
}  // namespace

extern "C" int cxx_restrict_name(void) {
    int restrict = 2;
    holder h{ restrict };
    return twice(h.restrict);
}
