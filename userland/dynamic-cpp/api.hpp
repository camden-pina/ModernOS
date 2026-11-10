#ifndef COAST_DYNAMIC_CPP_API_HPP
#define COAST_DYNAMIC_CPP_API_HPP
#include <stdexcept>
#include <string>
#include <vector>
struct CppError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Record {
    virtual ~Record();
    virtual std::string describe() const = 0;
};
struct Report : Record {
    int value;
    explicit Report(int value);
    ~Report() override;
    std::string describe() const override;
};
std::vector<int> parse_sorted(const std::string &);
Record *make_report(int);
void library_throw();
void library_call(void (*callback)());
int cleanup_count();
const void *library_tls(const std::string &);
int tls_destructors();
#endif
