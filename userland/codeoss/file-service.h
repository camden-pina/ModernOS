// Coast Code OSS integration. Original code; upstream licenses remain
// separate.
#pragma once
#include <stdexcept>
#include <string>
#include <sys/stat.h>
namespace CoreFS {
constexpr size_t MaxBytes = 4 * 1024 * 1024;
constexpr size_t MaxPathBytes = 512;
struct Failure : std::runtime_error {
  int number;
  std::string path, recovery;
  Failure(int code, const std::string &action, const std::string &target,
          const std::string &saved = {});
};
struct Contents {
  std::string bytes;
  mode_t mode;
};
struct SaveResult {
  std::string backup;
};
Contents read(const std::string &path);
SaveResult save(const std::string &path, const std::string &bytes,
                const std::string *expected);
std::u16string decode(const std::string &bytes);
std::string encode(const std::u16string &text);
const char *errorCode(int number);
} // namespace CoreFS
