#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static std::atomic<int> destroyed{0}, once_count{0}, caught{0};
struct Local {
  std::string value = "thread-local Coast";
  ~Local() { ++destroyed; }
};
static thread_local Local local;

int main() {
  std::mutex mutex;
  std::condition_variable condition;
  std::once_flag once;
  bool ready = false;
  unsigned total = 0;
  std::vector<std::thread> workers;
  for (unsigned i = 0; i != 4; ++i) {
    workers.emplace_back([&] {
      if (local.value != "thread-local Coast") std::terminate();
      if (std::this_thread::get_id() == std::thread::id()) std::terminate();
      std::call_once(once, [] { ++once_count; });
      std::unique_lock<std::mutex> lock(mutex);
      if (!condition.wait_for(lock, std::chrono::seconds(10), [&] { return ready; }))
        std::terminate();
      for (unsigned n = 0; n != 1000; ++n) ++total;
      lock.unlock();
      try { throw std::runtime_error("Coast exception"); }
      catch (const std::exception& error) {
        if (std::string(error.what()) != "Coast exception") std::terminate();
        ++caught;
      }
    });
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    ready = true;
  }
  condition.notify_all();
  for (auto& worker : workers) worker.join();
  std::shared_timed_mutex shared;
  shared.lock();
  auto future = std::async(std::launch::async, [&] {
    std::shared_lock<std::shared_timed_mutex> lock(shared, std::defer_lock);
    return lock.try_lock_for(std::chrono::seconds(10)) ? 42 : 0;
  });
  shared.unlock();
  if (future.get() != 42 || total != 4000 || once_count != 1 || caught != 4 || destroyed != 4)
    return 1;
  std::puts("CXX_THREADS_PASS mutex condition-variable call-once TLS destructors exceptions async shared-lock");
}
