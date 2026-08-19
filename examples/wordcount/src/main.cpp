// An ordinary program. It reads a file named on its command line, reports what
// it found, and starts another program.
//
// It uses global names for files, which the interface beneath it does not
// have; it uses an environment variable, arguments, a clock and a spawn. None
// of that is visible in the source, which is the property the arrangement
// exists to produce.
import openkal.libc;

namespace {

bool separator(unsigned char c) {
    return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

}  // namespace

extern "C" int main() {
    const auto started = kal::time::monotonic();

    if (kal::env::arg_count() < 2) {
        okc::print_line("usage: wordcount <file>");
        return 2;
    }
    kal_uintptr len = 0;
    const char* path = kal::env::arg(1, &len);

    auto text = okc::read_all(path, len);
    if (text.data == nullptr) {
        okc::print("cannot read ");
        okc::print(path, len);
        okc::print_line("");
        return 1;
    }

    kal_uintptr lines = 0, words = 0;
    bool in_word = false;
    for (kal_uintptr i = 0; i < text.size; ++i) {
        const unsigned char c = text.data[i];
        if (c == '\n') ++lines;
        if (separator(c)) { in_word = false; }
        else if (!in_word) { in_word = true; ++words; }
    }
    const kal_uintptr bytes = text.size;
    okc::release(text);

    okc::print("lines "); okc::print_unsigned(lines);
    okc::print(" words "); okc::print_unsigned(words);
    okc::print(" bytes "); okc::print_unsigned(bytes);
    okc::print_line("");

    // An environment variable, read through the interface rather than through a
    // global the C library would otherwise own.
    kal_uintptr vlen = 0;
    if (kal::env::var("WORDCOUNT_VERBOSE", 17, &vlen) != nullptr) {
        okc::print("elapsed ");
        okc::print_unsigned(kal::time::monotonic() - started);
        okc::print_line(" nanoseconds");
    }

    // Starting another program. The program is reached through a directory the
    // environment supplied, which is why the environment supplies more than one.
    for (kal_uintptr i = 0; i < kal::fs::preopen_count(); ++i) {
        kal_dir d{}; const char* n = nullptr; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, &n, &l) != kal_ok) continue;
        if (l != 1 || n[0] != '/') continue;
        kal_process p{};
        const char* argv[] = { "wordcount" };
        const kal_uintptr lens[] = { 9 };
        const char* candidates[] = { "bin/true", "usr/bin/true" };
        const kal_uintptr clens[] = { 8, 12 };
        for (int k = 0; k < 2; ++k) {
            if (kal_process_spawn(d, candidates[k], clens[k], argv, lens, 1,
                                  nullptr, nullptr, 0, nullptr, &p) != kal_ok) continue;
            int status = -1, terminated = -1;
            kal_process_wait(p, &status, &terminated);
            kal_process_close(p);
            okc::print("started a program, which finished with status ");
            okc::print_unsigned(static_cast<unsigned long long>(status));
            okc::print_line("");
            break;
        }
        break;
    }
    return 0;
}
