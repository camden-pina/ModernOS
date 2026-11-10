#include "api.hpp"
#include <cstdio>
namespace {
struct Lifetime {
    Lifetime() { std::puts("CPP PLUGIN CTOR PASS"); }
    ~Lifetime() { std::puts("CPP PLUGIN DTOR PASS"); }
} lifetime;
thread_local std::string text;
} // namespace
extern "C" const char *cpp_plugin_tls(const char *value) {
    text = value;
    return text.c_str();
}
extern "C" void cpp_plugin_throw() { library_throw(); }
