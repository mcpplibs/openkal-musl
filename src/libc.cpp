module openkal.libc;

namespace okc {

namespace {

kal_uintptr scan(const char* s) { kal_uintptr n = 0; while (s && s[n]) ++n; return n; }

bool prefix_of(const char* p, kal_uintptr plen, const char* s, kal_uintptr slen) {
    if (plen > slen) return false;
    for (kal_uintptr i = 0; i < plen; ++i) if (p[i] != s[i]) return false;
    return true;
}

}  // namespace

// The rule the specification places here rather than in itself. It is one rule,
// applied once, and it is what allows a program written against a global
// namespace to run above an interface that has none.
resolved resolve(const char* path, kal_uintptr len) {
    resolved r{ kal_dir{0}, path, len, false };
    if (path == nullptr || len == 0) return r;

    if (path[0] != '/') {
        // Relative: against the working directory, which is the first entry.
        kal_dir d{}; const char* n = nullptr; kal_uintptr l = 0;
        if (kal_fs_preopen(0, &d, &n, &l) != kal_ok) return r;
        r.dir = d; r.rest = path; r.rest_len = len; r.ok = true;
        return r;
    }

    // Absolute: the supplied entry whose name is the longest prefix. A program
    // confined to a subtree finds no entry for a name outside it, and therefore
    // fails to resolve it rather than reaching outside.
    kal_uintptr best = 0; bool found = false; kal_dir bestdir{};
    for (kal_uintptr i = 0; i < kal_fs_preopen_count(); ++i) {
        kal_dir d{}; const char* n = nullptr; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, &n, &l) != kal_ok) continue;
        if (n == nullptr || l == 0 || n[0] != '/') continue;
        if (!prefix_of(n, l, path, len)) continue;
        if (l >= best) { best = l; bestdir = d; found = true; }
    }
    if (!found) return r;

    kal_uintptr start = best;
    while (start < len && path[start] == '/') ++start;
    if (start >= len) {
        // The name denotes the supplied directory itself. openkal has no name
        // for that, so the caller receives the directory and an empty remainder.
        r.dir = bestdir; r.rest = path + len; r.rest_len = 0; r.ok = true;
        return r;
    }
    r.dir = bestdir; r.rest = path + start; r.rest_len = len - start; r.ok = true;
    return r;
}

file open_read(const char* path, kal_uintptr len) {
    file f{ kal_file{0}, kal_stream{0}, false };
    const auto r = resolve(path, len);
    if (!r.ok || r.rest_len == 0) return f;
    if (kal_fs_open_file(r.dir, r.rest, r.rest_len, 0, 0, &f.handle) != kal_ok) return f;
    f.stream = kal_stream{ kal_fs_stream(f.handle) };
    f.open = true;
    return f;
}

file open_write(const char* path, kal_uintptr len, bool create) {
    file f{ kal_file{0}, kal_stream{0}, false };
    const auto r = resolve(path, len);
    if (!r.ok || r.rest_len == 0) return f;
    if (kal_fs_open_file(r.dir, r.rest, r.rest_len, 1, create ? 1 : 0, &f.handle) != kal_ok) return f;
    f.stream = kal_stream{ kal_fs_stream(f.handle) };
    f.open = true;
    return f;
}

void close(file& f) {
    if (!f.open) return;
    kal_fs_close_file(f.handle);
    f.open = false;
}

blob read_all(const char* path, kal_uintptr len) {
    blob b{ nullptr, 0 };
    const auto r = resolve(path, len);
    if (!r.ok || r.rest_len == 0) return b;

    kal_node_info info{};
    if (kal_fs_info(r.dir, r.rest, r.rest_len, &info) != kal_ok) return b;
    if (info.kind != kal_node_file) return b;

    auto f = open_read(path, len);
    if (!f.open) return b;

    auto* data = static_cast<unsigned char*>(kal_alloc(info.size + 1, 8));
    if (data == nullptr) { close(f); return b; }

    kal_uintptr got = 0;
    while (got < info.size) {
        const auto rd = kal::read(f.stream, data + got, info.size - got);
        if (rd.e != kal_ok || rd.n == 0) break;
        got += rd.n;
    }
    close(f);
    data[got] = 0;
    b.data = data; b.size = got;
    return b;
}

void release(blob& b) {
    if (b.data == nullptr) return;
    kal_free(b.data, b.size + 1, 8);
    b.data = nullptr; b.size = 0;
}

void print(const char* s, kal_uintptr len) { kal::write(kal::out(), s, len); }
void print(const char* s)                  { print(s, scan(s)); }
void print_line(const char* s)             { print(s); print("\n", 1); }

void print_unsigned(unsigned long long v) {
    char buf[24];
    int i = 24;
    if (v == 0) buf[--i] = '0';
    while (v != 0) { buf[--i] = static_cast<char>('0' + (v % 10)); v /= 10; }
    print(buf + i, static_cast<kal_uintptr>(24 - i));
}

// A mutex built from the suspension primitive. The states are the ones the
// construction requires: free, held, and held while a context is suspended upon
// it. The third exists so that a release need not enter the environment when no
// context is waiting, which is the whole reason a program prefers this
// construction to a kernel object.
void mutex::lock() {
    __UINT32_TYPE__ expected = 0;
    if (__atomic_compare_exchange_n(&state, &expected, 1u, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    for (;;) {
        const auto previous = __atomic_exchange_n(&state, 2u, __ATOMIC_ACQUIRE);
        if (previous == 0) return;
        kal_task_wait(&state, 2u, 0);
    }
}

void mutex::unlock() {
    if (__atomic_exchange_n(&state, 0u, __ATOMIC_RELEASE) == 2u) {
        kal_uintptr woken = 0;
        kal_task_wake(&state, 1, &woken);
    }
}

}  // namespace okc
