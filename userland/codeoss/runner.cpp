// Dedicated Coast host for genuine upstream Code OSS text-core integration.
// Uses JavaScriptCore's public C API. Does not expose Node or a browser DOM.
#include "file-service.h"
#include <JavaScriptCore/JavaScript.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <vector>

struct Context {
  JSGlobalContextRef value;
  Context() : value(JSGlobalContextCreate(nullptr)) {
    if (!value)
      throw std::runtime_error("Cannot create JavaScriptCore context");
  }
  ~Context() { JSGlobalContextRelease(value); }
};
struct Root {
  JSContextRef context;
  JSValueRef value;
  Root(JSContextRef c, JSValueRef v) : context(c), value(v) {
    JSValueProtect(c, v);
  }
  ~Root() { JSValueUnprotect(context, value); }
  Root(const Root &) = delete;
  Root &operator=(const Root &) = delete;
};

static JSValueRef jsText(JSContextRef context, const std::u16string &text) {
  JSStringRef s = JSStringCreateWithCharacters(
      reinterpret_cast<const JSChar *>(text.data()), text.size());
  JSValueRef value = JSValueMakeString(context, s);
  JSStringRelease(s);
  return value;
}
static JSValueRef jsBytes(JSContextRef c, const std::string &text) {
  return jsText(c, CoreFS::decode(text));
}
static void property(JSContextRef c, JSObjectRef object, const char *key,
                     JSValueRef value) {
  Root root(c, value);
  JSStringRef k = JSStringCreateWithUTF8CString(key);
  JSObjectSetProperty(
      c, object, k, value,
      kJSPropertyAttributeReadOnly | kJSPropertyAttributeDontDelete, nullptr);
  JSStringRelease(k);
}
static std::u16string text(JSContextRef c, JSValueRef value) {
  if (!JSValueIsString(c, value))
    throw CoreFS::Failure(EINVAL, "Expected a string", "argument");
  JSValueRef exception = nullptr;
  JSStringRef s = JSValueToStringCopy(c, value, &exception);
  if (!s || exception)
    throw CoreFS::Failure(EINVAL, "Invalid string", "argument");
  auto chars = JSStringGetCharactersPtr(s);
  size_t length = JSStringGetLength(s);
  std::u16string result;
  if (length)
    result.assign(reinterpret_cast<const char16_t *>(chars), length);
  JSStringRelease(s);
  return result;
}
static std::string argument(JSContextRef c, size_t n, const JSValueRef *args,
                            size_t at) {
  if (at >= n)
    throw CoreFS::Failure(EINVAL, "Missing argument", "argument");
  return CoreFS::encode(text(c, args[at]));
}
static JSValueRef failure(JSContextRef c, JSValueRef *exception,
                          const std::exception &error) {
  JSValueRef message = jsBytes(c, error.what());
  Root messageRoot(c, message);
  JSObjectRef object = JSObjectMakeError(c, 1, &message, nullptr);
  Root errorRoot(c, object);
  if (auto e = dynamic_cast<const CoreFS::Failure *>(&error)) {
    property(c, object, "code", jsBytes(c, CoreFS::errorCode(e->number)));
    property(c, object, "errno", JSValueMakeNumber(c, e->number));
    property(c, object, "path", jsBytes(c, e->path));
    if (!e->recovery.empty())
      property(c, object, "recoveryPath", jsBytes(c, e->recovery));
  }
  *exception = object;
  return JSValueMakeUndefined(c);
}
#define CALLBACK(name)                                                         \
  static JSValueRef name(JSContextRef c, JSObjectRef, JSObjectRef, size_t n,   \
                         const JSValueRef args[], JSValueRef *exception)
CALLBACK(monotonicNow) {
  (void)n; (void)args;
  try {
    timespec clock;
    if(clock_gettime(CLOCK_MONOTONIC,&clock))
      throw CoreFS::Failure(errno,"Monotonic clock unavailable","clock");
    static bool initialized=false;
    static timespec origin;
    if(!initialized){origin=clock;initialized=true;}
    double elapsed=(clock.tv_sec-origin.tv_sec)*1000.0+(clock.tv_nsec-origin.tv_nsec)/1000000.0;
    return JSValueMakeNumber(c,elapsed);
  } catch(const std::exception &e) {return failure(c,exception,e);}
}
CALLBACK(readText) {
  try {
    return jsBytes(c, CoreFS::read(argument(c, n, args, 0)).bytes);
  } catch (const std::exception &e) {
    return failure(c, exception, e);
  }
}
CALLBACK(saveText) {
  try {
    auto path = argument(c, n, args, 0), bytes = argument(c, n, args, 1);
    if (n != 3)
      throw CoreFS::Failure(
          EINVAL, "saveText needs path, text and expected text/null", path);
    std::string expected;
    if (!JSValueIsNull(c, args[2]))
      expected = argument(c, n, args, 2);
    auto result = CoreFS::save(path, bytes,
                               JSValueIsNull(c, args[2]) ? nullptr : &expected);
    JSObjectRef object = JSObjectMake(c, nullptr, nullptr);
    Root resultRoot(c, object);
    property(c, object, "backupPath", jsBytes(c, result.backup));
    return object;
  } catch (const std::exception &e) {
    return failure(c, exception, e);
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
  } catch (const std::exception &e) {
    return failure(c, exception, e);
  }
}
CALLBACK(print) {
  try {
    for (size_t i = 0; i < n; i++) {
      JSStringRef s = JSValueToStringCopy(c, args[i], exception);
      if (!s || *exception)
        return JSValueMakeUndefined(c);
      auto data = JSStringGetCharactersPtr(s);
      std::u16string raw(reinterpret_cast<const char16_t *>(data),
                         JSStringGetLength(s));
      JSStringRelease(s);
      auto value = CoreFS::encode(raw);
      if (i)
        fputc(' ', stdout);
      fwrite(value.data(), 1, value.size(), stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return JSValueMakeUndefined(c);
  } catch (const std::exception &e) {
    return failure(c, exception, e);
  }
}
CALLBACK(readLine) {
  (void)n;
  (void)args;
  try {
    std::string line;
    for (;;) {
      int ch = fgetc(stdin);
      if (ch == EOF) {
        if (ferror(stdin))
          throw CoreFS::Failure(errno ? errno : EIO, "Read console", "stdin");
        if (line.empty())
          return JSValueMakeNull(c);
        break;
      }
      if (ch == '\n')
        break;
      if (line.size() >= 16384) {
        while (ch != '\n' && ch != EOF)
          ch = fgetc(stdin);
        throw CoreFS::Failure(EFBIG, "Console line exceeds 16 KiB", "stdin");
      }
      line += static_cast<char>(ch);
    }
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    return jsBytes(c, line);
  } catch (const std::exception &e) {
    return failure(c, exception, e);
  }
}
static void function(JSContextRef c, JSObjectRef o, const char *name,
                     JSObjectCallAsFunctionCallback call) {
  JSStringRef key = JSStringCreateWithUTF8CString(name);
  auto f = JSObjectMakeFunctionWithCallback(c, key, call);
  property(c, o, name, f);
  JSStringRelease(key);
}
int main(int argc, char **argv) {
  try {
    struct utsname os;
    if (uname(&os) || strcmp(os.sysname, "Coast")) {
      fputs("codeoss-core requires native Coast.\n", stderr);
      return 2;
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
      puts(
          "Usage: codeoss-core FILE\n       codeoss-core --script "
          "/ABSOLUTE/SCRIPT.js [args...]\nExperimental native upstream "
          "text-core CLI; UTF-8 files up to 4 MiB. Retains originals on save.");
      return 0;
    }
    const char *script = "/usr/share/codeoss/editor-cli.js";
    int first = 1;
    if (argc >= 2 && !strcmp(argv[1], "--script")) {
      if (argc < 3)
        throw std::runtime_error("--script requires an absolute script path");
      script = argv[2];
      first = 3;
    }
    auto source = CoreFS::decode(CoreFS::read(script).bytes);
    Context context;
    JSGlobalContextRef c = context.value;
    auto global = JSContextGetGlobalObject(c);
    auto host = JSObjectMake(c, nullptr, nullptr);
    Root hostRoot(c, host);
    property(c, host, "protocol", JSValueMakeNumber(c, 1));
    property(c, host, "platform", jsBytes(c, "coast"));
    property(c, host, "runtime", jsBytes(c, "JavaScriptCore"));
    property(c, host, "arch", jsBytes(c, "x64"));
    property(c, host, "maxTextBytes", JSValueMakeNumber(c, CoreFS::MaxBytes));
    property(c, host, "maxPathBytes", JSValueMakeNumber(c, CoreFS::MaxPathBytes));
    auto env = JSObjectMake(c, nullptr, nullptr);
    Root envRoot(c, env);
    for (const char *name : {"HOME", "LANG", "TMPDIR"})
      if (auto value = getenv(name))
        property(c, env, name, jsBytes(c, value));
    property(c, host, "env", env);
    auto arguments = JSObjectMakeArray(c, 0, nullptr, nullptr);
    Root argsRoot(c, arguments);
    for (int i = first; i < argc; i++)
      JSObjectSetPropertyAtIndex(c, arguments, i - first, jsBytes(c, argv[i]),
                                 nullptr);
    property(c, host, "args", arguments);
    function(c, host, "cwd", cwd);
    function(c, host, "readText", readText);
    function(c, host, "saveText", saveText);
    function(c, host, "readLine", readLine);
    property(c, global, "Coast", host);
    function(c, global, "print", print);
    auto console = JSObjectMake(c, nullptr, nullptr);
    Root consoleRoot(c, console);
    function(c, console, "log", print);
    function(c, console, "warn", print);
    function(c, console, "error", print);
    property(c, global, "console", console);
    auto performance = JSObjectMake(c,nullptr,nullptr);
    Root performanceRoot(c,performance);
    function(c,performance,"now",monotonicNow);
    property(c,global,"performance",performance);
    auto js = JSStringCreateWithCharacters(
        reinterpret_cast<const JSChar *>(source.data()), source.size());
    auto url = JSStringCreateWithUTF8CString(script);
    JSValueRef exception = nullptr;
    JSEvaluateScript(c, js, nullptr, url, 1, &exception);
    int result = 0;
    if (exception) {
      auto s = JSValueToStringCopy(c, exception, nullptr);
      size_t size = JSStringGetMaximumUTF8CStringSize(s);
      std::vector<char> message(size);
      JSStringGetUTF8CString(s, message.data(), size);
      fprintf(stderr, "codeoss-core: %s\n", message.data());
      JSStringRelease(s);
      result = 1;
    }
    JSStringRelease(js);
    JSStringRelease(url);
    return result;
  } catch (const std::exception &e) {
    fprintf(stderr, "codeoss-core: %s\n", e.what());
    return 1;
  }
}
