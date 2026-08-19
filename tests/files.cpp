// Files through the library, using global names, above an interface that has none.
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
    const char payload[] = "the library wrote this";

    auto w = okc::open_write("okc_probe.txt", 13, true);
    check(w.open, "a file opens for writing");
    check(kal::write(w.stream, payload, sizeof(payload) - 1).e == kal_ok, "it is written");
    okc::close(w);

    auto b = okc::read_all("okc_probe.txt", 13);
    check(b.data != nullptr, "the file reads back");
    check(b.size == sizeof(payload) - 1, "the whole file reads back");
    for (kal_uintptr i = 0; i < b.size; ++i)
        check(b.data[i] == static_cast<unsigned char>(payload[i]), "the contents match");
    okc::release(b);

    // Reading a name that does not exist reports nothing rather than failing to
    // return, and reading a directory does not produce a blob.
    auto missing = okc::read_all("okc_absent.txt", 14);
    check(missing.data == nullptr, "an absent file yields nothing");

    auto dir = okc::read_all("/", 1);
    check(dir.data == nullptr, "a directory does not read as a file");

    auto f = okc::open_read("okc_probe.txt", 13);
    check(f.open, "the file opens for reading");
    okc::close(f);

    // Removal goes through openkal directly, since the library offers no
    // operation for it and the specification's is already relative.
    auto r = okc::resolve("okc_probe.txt", 13);
    check(r.ok && kal_fs_remove(r.dir, r.rest, r.rest_len) == kal_ok, "the file is removed");

    okc::print_line("openkal-libc: files");
    return failures == 0 ? 0 : 1;
}
