#include "api.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <typeinfo>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "CPP CHECK FAIL line=%d\n", __LINE__);                            \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static void callback() { throw std::logic_error("main-callback"); }
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/dynamic-cpp-report.txt";
    auto values = parse_sorted("9,2,7,1,5");
    CHECK((values == std::vector<int>{1, 2, 5, 7, 9}));
    std::unique_ptr<Record> object(make_report(std::accumulate(values.begin(), values.end(), 0)));
    CHECK(object->describe() == "sum=24");
    CHECK(dynamic_cast<Report *>(object.get()) &&
          dynamic_cast<Report *>(object.get())->value == 24);
    CHECK(typeid(*object) == typeid(Report));
    try {
        library_throw();
        CHECK(false);
    } catch (const CppError &e) {
        CHECK(std::string(e.what()) == "cross-library");
    }
    library_call(callback);
    CHECK(cleanup_count() == 2);
    std::ofstream output(path);
    output << object->describe() << '\n';
    output.close();
    CHECK(output.good());
    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    CHECK(line == "sum=24");
    std::puts("CPP APPLICATION RTTI EXCEPTIONS PASS");
    std::mutex mutex;
    std::condition_variable condition;
    bool loaded = false;
    using tls_fn = const char *(*)(const char *);
    tls_fn plugin_tls = nullptr;
    std::atomic<int> good{0};
    const void *main_tls = library_tls("main");
    std::thread existing([&] {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return loaded; });
        lock.unlock();
        if (library_tls("worker") != main_tls &&
            std::strcmp(plugin_tls("worker plugin"), "worker plugin") == 0)
            ++good;
    });
    void *plugin = dlopen("libcpp-plugin.so", RTLD_NOW | RTLD_LOCAL);
    if (!plugin) {
        std::fprintf(stderr, "CPP DLOPEN FAIL %s\n", dlerror());
        std::terminate();
    }
    plugin_tls = reinterpret_cast<tls_fn>(dlsym(plugin, "cpp_plugin_tls"));
    CHECK(plugin_tls);
    CHECK(std::strcmp(plugin_tls("main plugin"), "main plugin") == 0);
    {
        std::lock_guard<std::mutex> lock(mutex);
        loaded = true;
    }
    condition.notify_one();
    existing.join();
    CHECK(good == 1 && tls_destructors() == 1);
    auto plugin_throw = reinterpret_cast<void (*)()>(dlsym(plugin, "cpp_plugin_throw"));
    CHECK(plugin_throw);
    try {
        plugin_throw();
        CHECK(false);
    } catch (const CppError &) {
    }
    CHECK(cleanup_count() == 3);
    dlerror();
    CHECK(!dlsym(plugin, "cpp_no_such_symbol") && dlerror());
    CHECK(!dlopen("libcpp-does-not-exist.so", RTLD_NOW) && dlerror());
    CHECK(!dlopen("libcpp-unresolved.so", RTLD_NOW) && dlerror());
    CHECK(dlclose(plugin) == 0);
    std::puts("CPP TLS DLOPEN ERRORS PASS");
    std::atomic<int> exceptions{0};
    std::vector<std::thread> racers;
    for (int i = 0; i < 4; i++)
        racers.emplace_back([&] {
            for (int j = 0; j < 32; j++) {
                try {
                    library_throw();
                } catch (const CppError &e) {
                    if (std::string(e.what()) == "cross-library")
                        ++exceptions;
                }
            }
        });
    for (auto &worker : racers)
        worker.join();
    CHECK(exceptions == 128 && cleanup_count() == 131);
    std::puts("CPP CONCURRENT UNWIND PASS");
    std::puts("DYNAMIC CPP CHECK PASS");
    return 0;
}
