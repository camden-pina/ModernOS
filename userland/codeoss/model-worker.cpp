// Original Coast process adapter for the genuine upstream Code OSS text
// core.
#include "../include/codeoss_protocol.h"
#include "file-service.h"
#include <JavaScriptCore/JavaScript.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <vector>
struct Context {
  JSGlobalContextRef c = JSGlobalContextCreate(nullptr);
  Context() {
    if (!c)
      throw std::runtime_error("Cannot create model context");
  }
  ~Context() { JSGlobalContextRelease(c); }
};
struct Root {
  JSContextRef c;
  JSValueRef v;
  Root(JSContextRef c, JSValueRef v) : c(c), v(v) {
    if (v)
      JSValueProtect(c, v);
  }
  ~Root() {
    if (v)
      JSValueUnprotect(c, v);
  }
  Root(const Root &) = delete;
};
static JSValueRef jsString(JSContextRef c, const std::u16string &s) {
  auto x = JSStringCreateWithCharacters(
      reinterpret_cast<const JSChar *>(s.data()), s.size());
  auto v = JSValueMakeString(c, x);
  JSStringRelease(x);
  return v;
}
static JSValueRef jsBytes(JSContextRef c, const std::string &s) {
  return jsString(c, CoreFS::decode(s));
}
static std::u16string text(JSContextRef c, JSValueRef v) {
  if (!JSValueIsString(c, v))
    throw std::runtime_error("Expected a model string");
  JSValueRef e = nullptr;
  auto s = JSValueToStringCopy(c, v, &e);
  if (!s || e)
    throw std::runtime_error("Cannot read model string");
  auto chars = JSStringGetCharactersPtr(s);
  std::u16string r;
  if (JSStringGetLength(s))
    r.assign(reinterpret_cast<const char16_t *>(chars), JSStringGetLength(s));
  JSStringRelease(s);
  return r;
}
static void put(JSContextRef c, JSObjectRef o, const char *key, JSValueRef v) {
  Root r(c, v);
  auto s = JSStringCreateWithUTF8CString(key);
  JSValueRef e = nullptr;
  JSObjectSetProperty(
      c, o, s, v, kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete,
      &e);
  JSStringRelease(s);
  if (e)
    throw std::runtime_error("Cannot initialize native host property");
}
static JSValueRef get(JSContextRef c, JSObjectRef o, const char *key) {
  auto s = JSStringCreateWithUTF8CString(key);
  JSValueRef e = nullptr;
  auto v = JSObjectGetProperty(c, o, s, &e);
  JSStringRelease(s);
  if (e)
    throw std::runtime_error("Cannot read model result");
  return v;
}
static std::string bytes(JSContextRef c, JSObjectRef o, const char *key) {
  auto v = get(c, o, key);
  Root r(c, v);
  return CoreFS::encode(text(c, v));
}
static uint32_t number(JSContextRef c, JSObjectRef o, const char *key) {
  auto v = get(c, o, key);
  if (!JSValueIsNumber(c, v))
    throw std::runtime_error("Invalid numeric model result");
  double n = JSValueToNumber(c, v, nullptr);
  if (!std::isfinite(n) || n < 0 || n > UINT32_MAX || floor(n) != n)
    throw std::runtime_error("Invalid numeric model range");
  return static_cast<uint32_t>(n);
}
static std::string argument(JSContextRef c, size_t n, const JSValueRef *args,
                            size_t at) {
  if (at >= n)
    throw std::runtime_error("Missing native argument");
  return CoreFS::encode(text(c, args[at]));
}
static JSValueRef failure(JSContextRef c, JSValueRef *e,
                          const std::exception &error) {
  auto message = jsBytes(c, error.what());
  Root m(c, message);
  auto o = JSObjectMakeError(c, 1, &message, nullptr);
  Root r(c, o);
  if (auto f = dynamic_cast<const CoreFS::Failure *>(&error)) {
    put(c, o, "code", jsBytes(c, CoreFS::errorCode(f->number)));
    put(c, o, "path", jsBytes(c, f->path));
    if (!f->recovery.empty())
      put(c, o, "recoveryPath", jsBytes(c, f->recovery));
  }
  *e = o;
  return JSValueMakeUndefined(c);
}
#define CALLBACK(name)                                                         \
  static JSValueRef name(JSContextRef c, JSObjectRef, JSObjectRef, size_t n,   \
                         const JSValueRef args[], JSValueRef *e)
CALLBACK(readText) {
  try {
    return jsBytes(c, CoreFS::read(argument(c, n, args, 0)).bytes);
  } catch (const std::exception &x) {
    return failure(c, e, x);
  }
}
CALLBACK(saveText) {
  try {
    if (n != 3)
      throw std::runtime_error("saveText needs path, text, expected/null");
    auto path = argument(c, n, args, 0), value = argument(c, n, args, 1);
    std::string old;
    if (!JSValueIsNull(c, args[2]))
      old = argument(c, n, args, 2);
    auto saved =
        CoreFS::save(path, value, JSValueIsNull(c, args[2]) ? nullptr : &old);
    auto o = JSObjectMake(c, nullptr, nullptr);
    Root r(c, o);
    put(c, o, "backupPath", jsBytes(c, saved.backup));
    return o;
  } catch (const std::exception &x) {
    return failure(c, e, x);
  }
}
CALLBACK(cwd) {
  (void)n;
  (void)args;
  try {
    char path[4096];
    if (!getcwd(path, sizeof(path)))
      throw CoreFS::Failure(errno, "Current directory", ".");
    return jsBytes(c, path);
  } catch (const std::exception &x) {
    return failure(c, e, x);
  }
}
CALLBACK(readLine) {
  (void)n;
  (void)args;
  return failure(
      c, e,
      CoreFS::Failure(ENOTSUP,
                      "Graphical model does not expose console-line input",
                      "stdin"));
}
CALLBACK(clockNow) {
  (void)n;
  (void)args;
  try {
    timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
      throw CoreFS::Failure(errno, "Monotonic clock", "clock");
    return JSValueMakeNumber(c, t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0);
  } catch (const std::exception &x) {
    return failure(c, e, x);
  }
}
CALLBACK(logMessage) {
  try {
    for (size_t i = 0; i < n; i++) {
      JSValueRef ex = nullptr;
      auto s = JSValueToStringCopy(c, args[i], &ex);
      if (!s || ex)
        throw std::runtime_error("Invalid model log string");
      std::u16string value(
          reinterpret_cast<const char16_t *>(JSStringGetCharactersPtr(s)),
          JSStringGetLength(s));
      JSStringRelease(s);
      auto out = CoreFS::encode(value);
      if (i)
        fputc(' ', stderr);
      fwrite(out.data(), 1, out.size(), stderr);
    }
    fputc('\n', stderr);
    return JSValueMakeUndefined(c);
  } catch (const std::exception &x) {
    return failure(c, e, x);
  }
}
static void function(JSContextRef c, JSObjectRef o, const char *key,
                     JSObjectCallAsFunctionCallback cb) {
  auto s = JSStringCreateWithUTF8CString(key);
  auto v = JSObjectMakeFunctionWithCallback(c, s, cb);
  put(c, o, key, v);
  JSStringRelease(s);
}
static void host(JSGlobalContextRef c, int argc, char **argv, int first) {
  auto global = JSContextGetGlobalObject(c),
       o = JSObjectMake(c, nullptr, nullptr);
  Root root(c, o);
  put(c, o, "protocol", JSValueMakeNumber(c, 1));
  put(c, o, "platform", jsBytes(c, "coast"));
  put(c, o, "runtime", jsBytes(c, "JavaScriptCore"));
  put(c, o, "arch", jsBytes(c, "x64"));
  put(c, o, "maxTextBytes", JSValueMakeNumber(c, CoreFS::MaxBytes));
  put(c, o, "maxPathBytes", JSValueMakeNumber(c, CoreFS::MaxPathBytes));
  auto env = JSObjectMake(c, nullptr, nullptr);
  Root er(c, env);
  for (const char *s : {"HOME", "LANG", "TMPDIR"})
    if (auto v = getenv(s))
      put(c, env, s, jsBytes(c, v));
  put(c, o, "env", env);
  auto a = JSObjectMakeArray(c, 0, nullptr, nullptr);
  Root ar(c, a);
  for (int i = first; i < argc; i++)
    JSObjectSetPropertyAtIndex(c, a, i - first, jsBytes(c, argv[i]), nullptr);
  put(c, o, "args", a);
  function(c, o, "cwd", cwd);
  function(c, o, "readText", readText);
  function(c, o, "saveText", saveText);
  function(c, o, "readLine", readLine);
  put(c, global, "Coast", o);
  auto console = JSObjectMake(c, nullptr, nullptr);
  Root cr(c, console);
  for (auto s : {"log", "warn", "error"})
    function(c, console, s, logMessage);
  put(c, global, "console", console);
  function(c, global, "print", logMessage);
  auto perf = JSObjectMake(c, nullptr, nullptr);
  Root pr(c, perf);
  function(c, perf, "now", clockNow);
  put(c, global, "performance", perf);
}
static void evaluate(JSGlobalContextRef c, const char *path) {
  auto source = CoreFS::decode(CoreFS::read(path).bytes);
  auto js = JSStringCreateWithCharacters(
           reinterpret_cast<const JSChar *>(source.data()), source.size()),
       url = JSStringCreateWithUTF8CString(path);
  JSValueRef e = nullptr;
  JSEvaluateScript(c, js, nullptr, url, 1, &e);
  JSStringRelease(js);
  JSStringRelease(url);
  if (e) {
    auto s = JSValueToStringCopy(c, e, nullptr);
    std::u16string raw(
        reinterpret_cast<const char16_t *>(JSStringGetCharactersPtr(s)),
        JSStringGetLength(s));
    JSStringRelease(s);
    throw std::runtime_error(CoreFS::encode(raw));
  }
}
static bool readAll(void *target, size_t size, bool eof = false) {
  auto p = static_cast<char *>(target);
  size_t done = 0;
  while (done < size) {
    ssize_t n = read(0, p + done, size - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      throw CoreFS::Failure(errno, "Read model request", "pipe");
    if (!n) {
      if (!done && eof)
        return false;
      throw std::runtime_error("Truncated model request");
    }
    done += n;
  }
  return true;
}
static void writeAll(const void *data, size_t size) {
  auto p = static_cast<const char *>(data);
  size_t done = 0;
  while (done < size) {
    ssize_t n = write(1, p + done, size - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw CoreFS::Failure(n < 0 ? errno : EIO, "Write model response",
                            "pipe");
    done += n;
  }
}
static void serve(JSGlobalContextRef c) {
  auto mv = get(c, JSContextGetGlobalObject(c), "codeOSSService");
  Root modelRoot(c, mv);
  if (!JSValueIsObject(c, mv))
    throw std::runtime_error("Graphical model missing");
  auto model = JSValueToObject(c, mv, nullptr);
  auto fv = get(c, model, "dispatch");
  Root functionRoot(c, fv);
  if (!JSValueIsObject(c, fv))
    throw std::runtime_error("Model dispatcher missing");
  auto dispatch = JSValueToObject(c, fv, nullptr);
  uint32_t sequence = 0;
  for (;;) {
    code_request r{};
    if (!readAll(&r, sizeof(r), true))
      break;
    if (r.magic != CODE_MAGIC || r.version != CODE_VERSION || !r.sequence ||
        r.sequence != sequence + 1 || r.action > CODE_RELEASE ||
        r.flags & ~31u || r.bytes > CODE_TEXT_LIMIT)
      throw std::runtime_error("Invalid bounded model request");
    sequence = r.sequence;
    std::string payload(r.bytes, '\0');
    if (r.bytes)
      readAll(payload.data(), payload.size());
    auto decoded = CoreFS::decode(payload);
    if (r.action == CODE_QUIT) {
      if (r.c)
        throw std::runtime_error("Invalid service shutdown identity");
      break;
    }
    if (!r.c || r.c > 0x7fffffff)
      throw std::runtime_error("Invalid document identity");
    auto request = JSObjectMake(c, nullptr, nullptr);
    Root requestRoot(c, request);
    put(c, request, "action", JSValueMakeNumber(c, r.action));
    put(c, request, "flags", JSValueMakeNumber(c, r.flags));
    put(c, request, "a", JSValueMakeNumber(c, r.a));
    put(c, request, "b", JSValueMakeNumber(c, r.b));
    put(c, request, "c", JSValueMakeNumber(c, r.c));
    put(c, request, "d", JSValueMakeNumber(c, r.d));
    put(c, request, "text", jsString(c, decoded));
    JSValueRef arg = request, e = nullptr;
    auto value = JSObjectCallAsFunction(c, dispatch, model, 1, &arg, &e);
    Root resultRoot(c, value);
    if (e || !JSValueIsObject(c, value))
      throw std::runtime_error("Graphical dispatcher failed");
    auto result = JSValueToObject(c, value, nullptr);
    code_response response{CODE_MAGIC,
                           CODE_VERSION,
                           r.sequence,
                           r.action,
                           number(c, result, "error"),
                           number(c, result, "flags"),
                           number(c, result, "cursor"),
                           number(c, result, "anchor"),
                           number(c, result, "lines"),
                           number(c, result, "line"),
                           number(c, result, "column"),
                           number(c, result, "revision"),
                           0,
                           0,
                           0,
                           0,
                           number(c, result, "eol"),
                           number(c, result, "tab"),
                           r.c};
    std::string path = bytes(c, result, "path"),
                content = bytes(c, result, "content"),
                selected = bytes(c, result, "selection"),
                message = bytes(c, result, "message");
    if (path.size() >= CODE_PATH_LIMIT || content.size() > CODE_TEXT_LIMIT ||
        selected.size() > CODE_CLIP_LIMIT ||
        message.size() > CODE_MESSAGE_LIMIT || response.status > 1 ||
        response.flags & ~63u || !response.lines ||
        response.line >= response.lines || response.cursor > CODE_TEXT_LIMIT ||
        response.anchor > CODE_TEXT_LIMIT || !response.revision ||
        (response.eol != 1 && response.eol != 2) ||
        (response.tab != 2 && response.tab != 4 && response.tab != 8))
      throw std::runtime_error("Invalid bounded model result");
    response.path_bytes = path.size();
    response.text_bytes = content.size();
    response.selection_bytes = selected.size();
    response.message_bytes = message.size();
    writeAll(&response, sizeof(response));
    for (auto *s : {&path, &content, &selected, &message})
      if (!s->empty())
        writeAll(s->data(), s->size());
  }
}
int main(int argc, char **argv) {
  try {
    utsname os;
    if (uname(&os) || strcmp(os.sysname, "Coast"))
      throw std::runtime_error("codeoss-model requires native Coast");
    bool check = argc >= 3 && !strcmp(argv[1], "--check");
    if (argc > 1 && !check)
      throw std::runtime_error(
          "Usage: codeoss-model [--check SCRIPT.js [args...]]");
    Context context;
    host(context.c, argc, argv, check ? 3 : argc);
    evaluate(context.c, "/usr/share/codeoss/graphical-model.js");
    if (check) {
      evaluate(context.c, argv[2]);
      puts("CODEOSS_GRAPHICAL_MODEL_CHECK_PASS");
    } else
      serve(context.c);
    return 0;
  } catch (const std::exception &e) {
    fprintf(stderr, "codeoss-model: %s\n", e.what());
    return 1;
  }
}
