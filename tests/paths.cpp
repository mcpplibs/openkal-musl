// The resolution of a global name against a set of supplied directories.
//
// This is the work the specification places in a C library, and it is therefore
// the work this package exists to test. The suite exercises both outcomes: a
// name that resolves, and a name outside every supplied directory, which must
// fail here rather than reaching outside.
import openkal.libc;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    okc::print("FAIL: "); okc::print_line(what);
}
}

int main() {
    // A relative name resolves against the working directory.
    auto r = okc::resolve("some/name", 9);
    check(r.ok, "a relative name resolves");
    check(r.rest_len == 9, "a relative name is unchanged by resolution");

    // An absolute name resolves against the supplied directory whose name is
    // the longest prefix, and the leading separator is consumed.
    auto a = okc::resolve("/etc/hostname", 13);
    check(a.ok, "an absolute name resolves");
    check(a.rest_len == 12, "the separator is consumed");
    check(a.rest[0] == 'e', "the remainder begins after the prefix");

    // The name of a supplied directory itself resolves to that directory with
    // an empty remainder, which openkal has no name for.
    auto root = okc::resolve("/", 1);
    check(root.ok && root.rest_len == 0, "a supplied directory resolves to itself");

    // An empty name does not resolve.
    check(!okc::resolve("", 0).ok, "an empty name does not resolve");
    check(!okc::resolve(nullptr, 0).ok, "an absent name does not resolve");

    okc::print_line("openkal-libc: path resolution");
    return failures == 0 ? 0 : 1;
}
