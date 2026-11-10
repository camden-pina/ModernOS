#include "file-service.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace CoreFS {
Failure::Failure(int code, const std::string &action, const std::string &target,
                 const std::string &saved)
    : std::runtime_error(action + ": " + strerror(code) + " (" + target + ")"),
      number(code), path(target), recovery(saved) {}
const char *errorCode(int n) {
  switch (n) {
  case ENOSYS:
    return "ENOSYS";
  case EPERM:
    return "EPERM";
  case ENAMETOOLONG:
    return "ENAMETOOLONG";
  case EXDEV:
    return "EXDEV";
  case ENOENT:
    return "ENOENT";
  case EEXIST:
    return "EEXIST";
  case EROFS:
    return "EROFS";
  case EACCES:
    return "EACCES";
  case EINVAL:
    return "EINVAL";
  case EILSEQ:
    return "EILSEQ";
  case EFBIG:
    return "EFBIG";
  case EISDIR:
    return "EISDIR";
  case ELOOP:
    return "ELOOP";
  case ENOTSUP:
    return "ENOTSUP";
  case ENOSPC:
    return "ENOSPC";
  case ENOMEM:
    return "ENOMEM";
  case EAGAIN:
    return "ECONFLICT";
  case ENOTDIR:
    return "ENOTDIR";
  default:
    return "EIO";
  }
}
struct FD {
  int value;
  explicit FD(int v) : value(v) {}
  ~FD() {
    if (value >= 0)
      close(value);
  }
};
static void pathCheck(const std::string &path) {
  if (path.size() >= MaxPathBytes)
    throw Failure(ENAMETOOLONG, "Native path exceeds 511 bytes", path);
  if (path.empty() || path[0] != '/' ||
      path.find('\0') != std::string::npos)
    throw Failure(EINVAL, "Expected an absolute native path", path);
  for (unsigned char c : path)
    if (c < 32 || c == 127)
      throw Failure(EINVAL, "Control characters in paths are unsupported",
                    "path");
}
Contents read(const std::string &path) {
  pathCheck(path);
  struct stat before;
  if (lstat(path.c_str(), &before))
    throw Failure(errno, "Check text", path);
  if (S_ISLNK(before.st_mode))
    throw Failure(ELOOP, "Symlink text targets are unsupported", path);
  if (!S_ISREG(before.st_mode))
    throw Failure(S_ISDIR(before.st_mode) ? EISDIR : ENOTSUP,
                  "Only regular text files are supported", path);
  FD fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (fd.value < 0)
    throw Failure(errno, "Open text", path);
  struct stat st;
  if (fstat(fd.value, &st))
    throw Failure(errno, "Stat text", path);
  if (!S_ISREG(st.st_mode))
    throw Failure(S_ISDIR(st.st_mode) ? EISDIR : ENOTSUP,
                  "Only regular text files are supported", path);
  if (st.st_size < 0 || static_cast<unsigned long long>(st.st_size) > MaxBytes)
    throw Failure(EFBIG, "Text file exceeds 4 MiB", path);
  Contents result{{}, st.st_mode};
  result.bytes.reserve(st.st_size);
  char block[8192];
  for (;;) {
    ssize_t n = ::read(fd.value, block, sizeof(block));
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      throw Failure(errno, "Read text", path);
    if (!n)
      break;
    if (result.bytes.size() + n > MaxBytes)
      throw Failure(EFBIG, "Text file exceeds 4 MiB", path);
    result.bytes.append(block, n);
  }
  struct stat after;
  if (fstat(fd.value, &after))
    throw Failure(errno, "Recheck text", path);
  if (st.st_size != after.st_size ||
      st.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      st.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
    throw Failure(EAGAIN, "File changed during read; retry", path);
  return result;
}
std::u16string decode(const std::string &bytes) {
  std::u16string text;
  text.reserve(bytes.size());
  for (size_t i = 0; i < bytes.size();) {
    unsigned c = static_cast<unsigned char>(bytes[i++]), value = c, count = 0,
             minimum = 0;
    if (!c)
      throw Failure(EILSEQ, "NUL/binary file is unsupported", "text");
    if (c >= 0xc2 && c <= 0xdf) {
      value = c & 31;
      count = 1;
      minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      value = c & 15;
      count = 2;
      minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      value = c & 7;
      count = 3;
      minimum = 0x10000;
    } else if (c >= 0x80)
      throw Failure(EILSEQ, "Invalid UTF-8", "text");
    if (i + count > bytes.size())
      throw Failure(EILSEQ, "Truncated UTF-8", "text");
    while (count--) {
      unsigned part = static_cast<unsigned char>(bytes[i++]);
      if ((part & 0xc0) != 0x80)
        throw Failure(EILSEQ, "Invalid UTF-8 continuation", "text");
      value = (value << 6) | (part & 63);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
      throw Failure(EILSEQ, "Invalid UTF-8 scalar", "text");
    if (value <= 0xffff)
      text.push_back(value);
    else {
      value -= 0x10000;
      text.push_back(0xd800 + (value >> 10));
      text.push_back(0xdc00 + (value & 1023));
    }
  }
  return text;
}
std::string encode(const std::u16string &text) {
  std::string bytes;
  for (size_t i = 0; i < text.size(); i++) {
    unsigned c = text[i];
    if (!c)
      throw Failure(EILSEQ, "NUL/binary text is unsupported", "text");
    if (c >= 0xd800 && c <= 0xdbff) {
      if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)
        throw Failure(EILSEQ, "Unpaired UTF-16 surrogate", "text");
      c = 0x10000 + ((c - 0xd800) << 10) + text[i] - 0xdc00;
    } else if (c >= 0xdc00 && c <= 0xdfff)
      throw Failure(EILSEQ, "Unpaired UTF-16 surrogate", "text");
    if (c < 0x80)
      bytes.push_back(c);
    else if (c < 0x800) {
      bytes.push_back(0xc0 | (c >> 6));
      bytes.push_back(0x80 | (c & 63));
    } else if (c < 0x10000) {
      bytes.push_back(0xe0 | (c >> 12));
      bytes.push_back(0x80 | ((c >> 6) & 63));
      bytes.push_back(0x80 | (c & 63));
    } else {
      bytes.push_back(0xf0 | (c >> 18));
      bytes.push_back(0x80 | ((c >> 12) & 63));
      bytes.push_back(0x80 | ((c >> 6) & 63));
      bytes.push_back(0x80 | (c & 63));
    }
    if (bytes.size() > MaxBytes)
      throw Failure(EFBIG, "Text exceeds 4 MiB", "text");
  }
  return bytes;
}
static int noReplace(const std::string &from, const std::string &to) {
  return syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(),
                 1);
}
SaveResult save(const std::string &path, const std::string &bytes,
                const std::string *expected) {
  pathCheck(path);
  if (bytes.size() > MaxBytes)
    throw Failure(EFBIG, "Text exceeds 4 MiB", path);
  // Validate before creating a file. Native callers share the same UTF-8
  // contract.
  decode(bytes);
  auto slash = path.find_last_of('/');
  std::string parent = path.substr(0, slash + 1), name = path.substr(slash + 1);
  if (name.empty() || name == "." || name == "..")
    throw Failure(EINVAL, "Expected a file name", path);
  mode_t mode = 0600;
  if (expected) {
    auto current = read(path);
    if (current.bytes != *expected)
      throw Failure(EAGAIN, "File changed since open; keep edits or Save As",
                    path);
    if (!(current.mode & 0222))
      throw Failure(EACCES, "Read-only file", path);
    mode = current.mode & 0777;
  } else {
    struct stat st;
    if (!lstat(path.c_str(), &st))
      throw Failure(EEXIST, "Save As preserves existing destinations", path);
    if (errno != ENOENT)
      throw Failure(errno, "Check destination", path);
  }
  static unsigned counter;
  std::string temporary, backup;
  FD fd(-1);
  for (unsigned attempt = 0; attempt < 1024; attempt++) {
    std::string suffix =
        std::to_string(getpid()) + "-" + std::to_string(++counter);
    temporary = parent + ".codeoss-tmp-" + suffix;
    backup = parent + ".codeoss-original-" + suffix;
    struct stat st;
    if (!lstat(backup.c_str(), &st))
      continue;
    if (errno != ENOENT)
      throw Failure(errno, "Check retained original name", path);
    fd.value = open(temporary.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd.value >= 0)
      break;
    if (errno != EEXIST)
      throw Failure(errno, "Create temporary text", path);
  }
  if (fd.value < 0)
    throw Failure(EEXIST, "No unused save temporary name", path);
  bool moved = false, published = false;
  try {
    for (size_t pos = 0; pos < bytes.size();) {
      ssize_t n = write(fd.value, bytes.data() + pos, bytes.size() - pos);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        throw Failure(n < 0 ? errno : EIO, "Write temporary text", path);
      pos += n;
    }
    if (fchmod(fd.value, mode))
      throw Failure(errno, "Preserve text permissions", path);
    if (fsync(fd.value))
      throw Failure(errno, "Sync temporary text", path);
    if (close(fd.value)) {
      fd.value = -1;
      throw Failure(errno, "Close temporary text", path);
    }
    fd.value = -1;
    if (expected) {
      if (noReplace(path, backup))
        throw Failure(errno, "Retain original", path);
      moved = true;
      if (read(backup).bytes != *expected)
        throw Failure(EAGAIN, "File changed before save; original retained",
                      path);
    }
    if (noReplace(temporary, path))
      throw Failure(errno, "Publish text without clobbering a new destination",
                    path);
    published = true;
    // Originals deliberately remain recoverable in this experimental milestone.
    return {moved ? backup : ""};
  } catch (const Failure &e) {
    int number = e.number;
    std::string recovery;
    if (fd.value >= 0) {
      close(fd.value);
      fd.value = -1;
    }
    if (moved && noReplace(backup, path))
      recovery = backup;
    if (!published)
      unlink(temporary.c_str());
    throw Failure(number, "Save text failed; unsaved model retained", path,
                  recovery);
  } catch (const std::exception &) {
    if (fd.value >= 0) {
      close(fd.value);
      fd.value = -1;
    }
    std::string recovery;
    if (moved && noReplace(backup, path))
      recovery = backup;
    unlink(temporary.c_str());
    throw Failure(ENOMEM, "Save could not complete; unsaved model retained",
                  path, recovery);
  }
}
} // namespace CoreFS
