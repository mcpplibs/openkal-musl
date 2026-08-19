// openkal.libc --- the surface a program written for a hosted system expects,
// implemented above openkal.
//
// The package exists to test a claim rather than to replace a C library. The
// claim is that porting one library causes the software above it to run on
// every implementation of openkal, and the claim is only testable if such a
// library exists.
//
// Two pieces of work are performed here deliberately, because the specification
// places them here and not in itself.
//
// The first is the resolution of an absolute path. openkal has no global
// namespace of names: every operation is relative to a directory the
// environment supplied. A program written for a hosted system does not know
// that, and does not have to: this library selects the supplied directory whose
// name is the longest prefix of the path and opens the remainder relative to
// it. That is one implementation of one rule, in one place, rather than the
// same rule in every program.
//
// The second is the construction of the synchronisation objects a program
// expects from the suspension primitive openkal provides. The primitive is what
// an environment can supply; a mutex is what a program asks for.
export module openkal.libc;
export import openkal.types;
export import openkal.stream;
export import openkal.memory;
export import openkal.fs;
export import openkal.process;
export import openkal.task;
export import openkal.time;
export import openkal.env;

export namespace okc {

// --- Paths ------------------------------------------------------------------

// The result of resolving a name against the directories the environment
// supplied. `dir` is borrowed from the supplied set and is not released;
// `rest` points into the caller's own name.
struct resolved {
    kal_dir      dir;
    const char*  rest;
    kal_uintptr  rest_len;
    bool         ok;
};

// Selects the supplied directory whose name is the longest prefix of `path`,
// and returns the remainder relative to it.
//
// A relative path resolves against the working directory, which is the first
// entry the environment supplies. An absolute path resolves against the entry
// whose name is the longest prefix, so that a program confined to a subtree
// fails to resolve a name outside it rather than reaching outside it.
resolved resolve(const char* path, kal_uintptr len);

// --- Files ------------------------------------------------------------------

struct file {
    kal_file   handle;
    kal_stream stream;
    bool       open;
};

file open_read (const char* path, kal_uintptr len);
file open_write(const char* path, kal_uintptr len, bool create);
void close(file&);

// Reads the whole of a file into memory obtained from openkal.memory. The
// caller releases it with `release`.
struct blob { unsigned char* data; kal_uintptr size; };
blob read_all(const char* path, kal_uintptr len);
void release(blob&);

// --- Output -----------------------------------------------------------------

void print(const char* s, kal_uintptr len);
void print(const char* s);                  // counted by scanning
void print_line(const char* s);
void print_unsigned(unsigned long long v);

// --- Synchronisation --------------------------------------------------------
//
// Built from the suspension primitive, which is the relation a C library has to
// a kernel on a system that provides one. openkal declares the primitive for
// that reason.
struct mutex {
    __UINT32_TYPE__ state = 0;   // 0 free, 1 held, 2 held and contended
    void lock();
    void unlock();
};

}  // namespace okc
