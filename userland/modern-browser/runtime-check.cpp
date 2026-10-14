#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL C++ runtime:%d: %s\n",__LINE__,#x); std::exit(1); } } while (0)

int main() {
    std::mutex mutex;
    std::condition_variable condition;
    std::once_flag once;
    std::atomic<int> initialized{0};
    std::atomic<int> exceptions{0};
    bool run = false;
    int arrived = 0, count = 0, exception_ready = 0;
    std::vector<std::thread> threads;
    std::promise<int> promise;
    auto result = promise.get_future();
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i] {
            std::call_once(once, [&] { ++initialized; });
            std::unique_lock guard(mutex);
            ++arrived;
            condition.notify_all();
            condition.wait(guard, [&] { return run; });
            guard.unlock();
            for (int j = 0; j < 300; ++j) {
                std::lock_guard hold(mutex);
                ++count;
            }
            // Start the first unwind concurrently, including the lazy frame
            // registration/sorting path, then exercise repeated exceptions.
            guard.lock();
            ++exception_ready;
            condition.notify_all();
            condition.wait(guard, [&] { return exception_ready == 4; });
            guard.unlock();
            for (int j = 0; j < 32; ++j) {
                try {
                    throw std::runtime_error("worker exception");
                } catch (std::runtime_error const &) {
                    ++exceptions;
                    if (!i && !j)
                        promise.set_value(73);
                }
            }
        });
    }

    {
        std::unique_lock guard(mutex);
        condition.wait(guard, [&] { return arrived == 4; });
        run = true;
        condition.notify_all();
    }

    CHECK(result.get() == 73);
    for (auto &thread : threads)
        thread.join();
    CHECK(count == 1200 && initialized == 1);
    CHECK(exceptions == 128);
    std::puts("CXX THREAD CHECK PASS");
}
