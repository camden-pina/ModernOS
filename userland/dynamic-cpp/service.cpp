#include "api.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <sstream>
namespace {
std::atomic<int> cleaned{0}, tls_cleaned{0};
struct Lifecycle {
    Lifecycle() { std::puts("CPP LIB CTOR PASS"); }
    ~Lifecycle() { std::puts("CPP LIB DTOR PASS"); }
} lifecycle;
struct Local {
    std::string value;
    ~Local() { ++tls_cleaned; }
};
thread_local Local local;
struct Cleanup {
    ~Cleanup() { ++cleaned; }
};
} // namespace
Record::~Record() = default;
Report::Report(int n) : value(n) {}
Report::~Report() = default;
std::string Report::describe() const { return "sum=" + std::to_string(value); }
std::vector<int> parse_sorted(const std::string &csv) {
    std::vector<int> values;
    std::istringstream input(csv);
    std::string part;
    while (std::getline(input, part, ',')) {
        std::size_t end = 0;
        int n = std::stoi(part, &end);
        if (end != part.size())
            throw CppError("invalid CSV field");
        values.push_back(n);
    }
    std::sort(values.begin(), values.end());
    return values;
}
Record *make_report(int value) { return new Report(value); }
void library_throw() {
    Cleanup cleanup;
    throw CppError("cross-library");
}
void library_call(void (*callback)()) {
    Cleanup cleanup;
    try {
        callback();
    } catch (const std::logic_error &e) {
        if (std::string(e.what()) != "main-callback")
            throw;
        std::puts("CPP CALLBACK EXCEPTION PASS");
    }
}
int cleanup_count() { return cleaned.load(); }
const void *library_tls(const std::string &value) {
    local.value = value;
    return &local;
}
int tls_destructors() { return tls_cleaned.load(); }
