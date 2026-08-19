// A mutex built from the suspension primitive openkal provides.
//
// The test is a contended one. An uncontended mutex never enters the
// environment, so a suite that exercised only that path would pass without the
// primitive working at all.
import openkal.libc;

namespace {

okc::mutex g_lock;
volatile long long g_counter = 0;
constexpr int kContexts = 4;
constexpr int kIncrements = 20000;

void worker(void*) {
    for (int i = 0; i < kIncrements; ++i) {
        g_lock.lock();
        g_counter = g_counter + 1;
        g_lock.unlock();
    }
}

}  // namespace

int main() {
    kal_task t[kContexts]{};
    for (auto& h : t)
        if (kal_task_start(worker, nullptr, &h) != kal_ok) {
            okc::print_line("FAIL: a context did not start");
            return 1;
        }
    for (auto& h : t) kal_task_join(h);

    const long long expected = static_cast<long long>(kContexts) * kIncrements;
    if (g_counter != expected) {
        okc::print("FAIL: counter is ");
        okc::print_unsigned(static_cast<unsigned long long>(g_counter));
        okc::print(" and should be ");
        okc::print_unsigned(static_cast<unsigned long long>(expected));
        okc::print_line("");
        return 1;
    }
    okc::print_line("openkal-libc: contended mutex above the suspension primitive");
    return 0;
}
