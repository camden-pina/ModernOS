// Native software browser view. The broker owns origin changes and network
// credentials; this process owns only its origin's DOM, input and client pixels.
#include "FocusController.h"
#include "CommonVM.h"
#include <JavaScriptCore/Debugger.h>
#include <JavaScriptCore/DebuggerCallFrame.h>
#include "DebuggerScope.h"
#include <JavaScriptCore/Breakpoint.h>
#include <JavaScriptCore/ErrorInstance.h>
#include <JavaScriptCore/JSCast.h>
#include <JavaScriptCore/JSArray.h>
#include <JavaScriptCore/JSObjectInlines.h>
#include <wtf/Scope.h>
#include <wtf/HashSet.h>
#include <wtf/HashMap.h>
#include <wtf/text/StringHash.h>
#include "DocumentPage.h"
#include "StringWithDirection.h"
#include "UserAgent.h"
#include "BrowserServices.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <pthread.h>
#include <poll.h>
#include <vector>
#include <string>
#include "BrowserHistory.h"
#include "BackForwardCache.h"
#include "FrameTreeSyncData.h"
#include "HTMLFrameOwnerElement.h"
#include "NodeDocument.h"
#include "FormData.h"
#include "HistoryController.h"
#include "LocalFrameLoaderClient.h"
#include "MemoryRelease.h"
#include "FontConfigModernOS.h"
#include "SystemSettings.h"
#include "ScreenModernOS.h"
#include "RemoteFrame.h"
#include "FrameInlines.h"
#include "ProcessIdentifier.h"
#include "RemoteFrameClient.h"
#include "RemoteFrameView.h"
#include "RemoteDOMWindow.h"
#include "LocalDOMWindow.h"
#include "RenderWidget.h"
#include "RenderBoxInlines.h"
#include "AccessibilityRemoteToken.h"
#include "SerializedScriptValue.h"
#include "NonSerializedDataToken.h"
#include "MessageWithMessagePorts.h"
#include "JSDOMGlobalObject.h"
#include "DOMWrapperWorld.h"
#include "WindowProxy.h"
#include <skia/core/SkImage.h>
#include <skia/core/SkData.h>
#include "NativeImage.h"
#include "Event.h"
#include "EventNames.h"
#include "../../include/memory_usage.h"
#include <sys/resource.h>

namespace {
static bool nativeExceptionTrace = false;
// Explicit developer diagnostics use JSC's real exception debugger. They
// observe the original exception and immediately continue, without executing
// a replacement script, overriding an API, or changing any clock.
class NativeExceptionDebugger final : public JSC::Debugger, private JSC::Debugger::Observer {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(NativeExceptionDebugger);

  public:
    explicit NativeExceptionDebugger(JSC::VM &vm) : JSC::Debugger(vm) {
        addObserver(*this);
        activateBreakpoints();
        setPauseOnAllExceptionsBreakpoint(JSC::Breakpoint::create(JSC::noBreakpointID));
    }

    ~NativeExceptionDebugger() final {
        removeObserver(*this, true);
    }

  private:
    HashSet<String> messages;
    HashMap<JSC::SourceID, String> sourceURLs;

    static bool identifier(const std::string &name) {
        if (name.empty() || name.size() > 64)
            return false;
        for (unsigned i = 0; i < name.size(); i++) {
            char c = name[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
                  (i && c >= '0' && c <= '9')))
                return false;
        }

        return true;
    }

    JSC::JSValue local(JSC::JSGlobalObject *global, JSC::DebuggerCallFrame &frame,
                       const std::string &name) {
        auto property = JSC::Identifier::fromString(vm(), String::fromUTF8(name.c_str()));
        unsigned depth = 0;
        for (auto *scope = frame.scope(vm()); scope && depth++ < 16; scope = scope->next()) {
            // These lexical scopes have direct VM bindings. Ignore global and
            // with scopes, which can wrap arbitrary user or platform objects.
            if (!scope->isValid() || scope->isGlobalScope() ||
                scope->isGlobalLexicalEnvironment() || scope->isWithScope())
                continue;
            JSC::PropertySlot slot(scope, JSC::PropertySlot::InternalMethodType::VMInquiry, &vm());
            if (JSC::DebuggerScope::getOwnPropertySlot(scope, global, property, slot) &&
                slot.isValue())
                // Lexical slots are deliberately uncacheable. getPureResult
                // returns null for them; getValue on a confirmed value slot
                // reads its stored JSValue and cannot invoke an accessor.
                return slot.getValue(global, property);
        }

        return {};
    }

    static String publicWebName(JSC::JSValue value) {
        auto *string = dynamicDowncast<JSC::JSString>(value);
        if (!string)
            return {};
        auto owned = string->tryGetValueWithoutGC();
        const auto &text = owned.data;
        // Never print arbitrary local strings, credentials, or decoded site
        // payloads. Only these public Web interface/property names are allowed.
        static constexpr const char *names[] = {"prototype",
                                                "constructor",
                                                "navigator",
                                                "window",
                                                "document",
                                                "screen",
                                                "AudioContext",
                                                "webkitAudioContext",
                                                "OfflineAudioContext",
                                                "AudioBuffer",
                                                "WebGLRenderingContext",
                                                "WebGL2RenderingContext",
                                                "OffscreenCanvas",
                                                "HTMLCanvasElement",
                                                "CanvasRenderingContext2D",
                                                "getContext",
                                                "webgl",
                                                "webgl2",
                                                "indexedDB",
                                                "IDBFactory",
                                                "IDBDatabase",
                                                "serviceWorker",
                                                "ServiceWorker",
                                                "SharedWorker",
                                                "Worker",
                                                "caches",
                                                "CacheStorage",
                                                "storage",
                                                "StorageManager",
                                                "localStorage",
                                                "sessionStorage",
                                                "permissions",
                                                "Permissions",
                                                "locks",
                                                "LockManager",
                                                "gpu",
                                                "GPU",
                                                "bluetooth",
                                                "usb",
                                                "serial",
                                                "hid",
                                                "userAgentData",
                                                "deviceMemory",
                                                "hardwareConcurrency",
                                                "webdriver",
                                                "plugins",
                                                "mimeTypes",
                                                "mediaDevices",
                                                "requestMediaKeySystemAccess",
                                                "SharedArrayBuffer",
                                                "WebAssembly",
                                                "crypto",
                                                "subtle",
                                                "geolocation",
                                                "credentials"};
        for (const char *name : names)
            if (text == String::fromUTF8(name))
                return text;
        return {};
    }

    static const char *kind(JSC::JSValue value) {
        if (!value)
            return "unavailable";
        if (value.isUndefined())
            return "undefined";
        if (value.isNull())
            return "null";
        if (value.isNumber())
            return "number";
        if (value.isBoolean())
            return "boolean";
        if (value.isString())
            return "string";
        if (dynamicDowncast<JSC::JSArray>(value))
            return "array";
        if (value.isObject())
            return "object";
        return "other";
    }

    void inspectIndexedAccess(JSC::JSGlobalObject *global, JSC::DebuggerCallFrame &frame,
                              const String &message) {
        std::string text(message.utf8().data());
        auto start = text.find("(evaluating '");
        if (start == std::string::npos ||
            text.find("undefined is not an object") == std::string::npos)
            return;
        start += sizeof("(evaluating '") - 1;
        auto end = text.find("')", start);
        if (end == std::string::npos)
            return;
        text = text.substr(start, end - start);
        // Inspect identifier[identifier][identifier...] expressions using
        // direct lexical bindings and dense array entries. No JS evaluation.
        std::vector<std::string> names;
        size_t at = 0;
        while (at < text.size() && names.size() < 5) {
            size_t end = at;
            while (end < text.size() && text[end] != '[' && text[end] != ']')
                end++;
            if (end > at) {
                auto name = text.substr(at, end - at);
                if (!identifier(name))
                    return;
                if (std::find(names.begin(), names.end(), name) == names.end())
                    names.push_back(name);
            }

            at = end + 1;
        }

        if (names.size() < 2)
            return;
        std::vector<JSC::JSValue> values;
        for (const auto &name : names) {
            auto value = local(global, frame, name);
            values.push_back(value);
            auto webName = publicWebName(value);
            fprintf(stderr, "JavaScript diagnostic local %s: %s%s%s\n", name.c_str(), kind(value),
                    webName.isEmpty() ? "" : " web-name=", webName.utf8().data());
        }

        for (unsigned n = 0; n < values.size(); n++) {
            auto *array = dynamicDowncast<JSC::JSArray>(values[n]);
            if (!array)
                continue;
            for (auto index : values) {
                if (!index.isUInt32())
                    continue;
                unsigned i = index.asUInt32();
                if (!array->canGetIndexQuickly(i))
                    continue;
                auto value = array->getIndexQuickly(i);
                auto webName = publicWebName(value);
                fprintf(stderr, "JavaScript diagnostic array %s[%u]: %s%s%s\n", names[n].c_str(), i,
                        kind(value), webName.isEmpty() ? "" : " web-name=", webName.utf8().data());
            }

            unsigned found = 0;
            for (unsigned i = 0; i < std::min<unsigned>(array->length(), 256) && found < 12; i++) {
                if (!array->canGetIndexQuickly(i))
                    continue;
                auto webName = publicWebName(array->getIndexQuickly(i));
                if (webName.isEmpty())
                    continue;
                fprintf(stderr, "JavaScript diagnostic web name %s[%u]=%s\n", names[n].c_str(), i,
                        webName.utf8().data());
                found++;
            }
        }
    }

    void didParseSource(JSC::SourceID id, const JSC::Debugger::Script &script) final {
        if (sourceURLs.size() < 256)
            sourceURLs.set(id, script.url.left(2048));
    }

    void didPause(JSC::JSGlobalObject *global, JSC::DebuggerCallFrame &pausedFrame,
                  JSC::JSValue exception) final {
        if (reasonForPause() == PausedForException && messages.size() < 128) {
            String text = "non-Error exception"_s, url;
            if (auto *error = dynamicDowncast<JSC::ErrorInstance>(exception))
                text = error->sanitizedToString(global);
            if (auto *dom = dynamicDowncast<JSDOMGlobalObject>(global))
                if (auto *context = dom->scriptExecutionContext())
                    url = context->url().string().left(2048);
            text = text.left(2048);
            // Feature checks can deliberately repeat the same caught error
            // hundreds of times. Keep the bounded budget for distinct errors.
            if (!messages.add(makeString(url, '\n', text)).isNewEntry)
                return;
            fprintf(stderr, "JavaScript diagnostic exception %s: %s\n", url.utf8().data(),
                    text.utf8().data());
            RefPtr<JSC::DebuggerCallFrame> frame = &pausedFrame;
            for (unsigned depth = 0; frame && depth < 3; depth++, frame = frame->callerFrame())
                fprintf(stderr, "JavaScript diagnostic frame %s %s:%d:%d\n",
                        frame->functionName(vm()).utf8().data(),
                        sourceURLs.get(frame->sourceID()).utf8().data(), frame->line() + 1,
                        frame->column() + 1);
            inspectIndexedAccess(global, pausedFrame, text);
        }
    }

    // The base handlePause dispatches observers, then enters this hook. Resume
    // here so its pause state is cleared after the observer notification.

    void runEventLoopWhilePaused() final {
        continueProgram();
    }
};

struct NativeEmbedding {
    bool enabled = false;
    uint64_t frame = 0, parent = 0;
    uint16_t sandbox = 0;
    URL parentOrigin, topOrigin;
    String referrer;
};

static NativeEmbedding nativeEmbedding;
static bool nativeClientNavigation = false;
struct NativeOutput {
    std::deque<std::vector<uint8_t>> messages;
    std::vector<uint8_t> previous;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
    pthread_t writer;
    bool started = false, writing = false;
    std::atomic<bool> broken{false}, stopping{false};
    int oldWidth = 0, oldHeight = 0;
    bool reportedBackingCopy = false;
    sk_sp<SkSurface> surface;

    ~NativeOutput() {
        stopping = true;
        CHECK(!pthread_mutex_lock(&mutex));
        CHECK(!pthread_cond_signal(&ready));
        CHECK(!pthread_mutex_unlock(&mutex));
        if (started)
            CHECK(!pthread_join(writer,nullptr));
        CHECK(!pthread_cond_destroy(&ready));
        CHECK(!pthread_mutex_destroy(&mutex));
    }

    void start() {
        CHECK(!started && !pthread_create(&writer,nullptr,[](void* data)->void* {
            static_cast<NativeOutput*>(data)->writeFrames();return nullptr;
        },this));
        started = true;
    }

    void queue(std::vector<uint8_t> &&buffer) {
        CHECK(!pthread_mutex_lock(&mutex));
        if (messages.size() >= 128)
            broken = true;
        else {
            messages.push_back(std::move(buffer));
            CHECK(!pthread_cond_signal(&ready));
        }
        CHECK(!pthread_mutex_unlock(&mutex));
    }

    void text(unsigned type, const String &value, unsigned flags = 0) {
        auto bytes = value.utf8();
        browser_packet packet = {};
        packet.magic = BROWSER_MAGIC;
        packet.type = type;
        packet.flags = flags;
        packet.bytes = std::min<size_t>(bytes.length(), BROWSER_MAX_TEXT);
        std::vector<uint8_t> buffer(sizeof(packet) + packet.bytes);
        memcpy(buffer.data(), &packet, sizeof(packet));
        if (packet.bytes)
            memcpy(buffer.data() + sizeof(packet), bytes.data(), packet.bytes);
        queue(std::move(buffer));
    }

    bool idle() {
        CHECK(!pthread_mutex_lock(&mutex));
        bool idle = !writing && messages.empty();
        CHECK(!pthread_mutex_unlock(&mutex));
        return idle;
    }

    void writeFrames() {
        // DOM/JSC remain on the main thread. This worker only transfers
        // immutable prepared bytes, so a long script cannot strand half of
        // an already painted surface in either nonblocking pixel pipe.
        while (!broken && !stopping) {
            CHECK(!pthread_mutex_lock(&mutex));
            while (messages.empty() && !stopping)
                CHECK(!pthread_cond_wait(&ready,&mutex));
            if (stopping) {
                CHECK(!pthread_mutex_unlock(&mutex));
                break;
            }
            auto buffer = std::move(messages.front());
            messages.pop_front();
            writing = true;
            CHECK(!pthread_mutex_unlock(&mutex));
            size_t sent = 0;
            while (sent < buffer.size() && !stopping && !broken) {
                ssize_t n = write(1, buffer.data() + sent, buffer.size() - sent);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    pollfd fd = {1, POLLOUT, 0};
                    if (poll(&fd, 1, 25) < 0 && errno != EINTR)
                        broken = true;
                    continue;
                }

                if (n <= 0) {
                    broken = true;
                    break;
                }
                sent += n;
            }

            CHECK(!pthread_mutex_lock(&mutex));
            writing = false;
            CHECK(!pthread_mutex_unlock(&mutex));
        }
    }

    void paint(LocalFrame &frame) {
        if (!frame.view())
            return;
        int width = viewState.width, height = viewState.height;
        if (!surface || surface->width() != width || surface->height() != height) {
            surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
            viewState.invalidateAll();
        }
        if (!surface) {
            text(BROWSER_NOTICE, "Not enough memory to paint this page."_s);
            return;
        }
        frame.view()->updateLayoutAndStyleIfNeededRecursive();
        bool repaintAll = viewState.repaintAll;
        viewState.repaintAll = false;
        auto scrollCopies = std::move(viewState.scrollCopies);
        viewState.scrollCopies.clear();
        auto repaint = std::move(viewState.damage);
        viewState.damage = {};
        viewState.dirty = false;
        if (!repaintAll && scrollCopies.empty() && repaint.isEmpty())
            return;
        if (repaintAll) {
            surface->getCanvas()->clear(SK_ColorWHITE);
            repaint = {IntRect(0, 0, width, height)};
        } else if (!scrollCopies.empty()) {
            if (!reportedBackingCopy) {
                fprintf(stderr, "Native backing scroll: WebCore retained raster copy\n");
                reportedBackingCopy = true;
            }
            surface->notifyContentWillChange(SkSurface::kRetain_ContentChangeMode);
            SkPixmap backing;
            CHECK(surface->peekPixels(&backing) && backing.rowBytes()==static_cast<size_t>(width)*4);
            for (const auto &copy : scrollCopies) {
                browser_packet packet = {};
                packet.magic = BROWSER_MAGIC;
                packet.type = BROWSER_COPY;
                packet.canvas_width = width;
                packet.canvas_height = height;
                packet.bytes = sizeof(browser_copy_source);
                packet.x = copy.destination.x();
                packet.y = copy.destination.y();
                packet.width = copy.destination.width();
                packet.height = copy.destination.height();
                browser_copy_source source = {copy.source.x(), copy.source.y()};
                CHECK(browser_surface_copy(static_cast<uint32_t*>(backing.writable_addr()),width,height,&packet,&source));
            }
        }

        GraphicsContextSkia context(*surface->getCanvas(), RenderingMode::Unaccelerated,
                                    RenderingPurpose::Unspecified);
        for (const auto &rect : repaint.rects()) {
            context.save();
            context.clip(rect);
            context.fillRect(rect, Color::white, CompositeOperator::Copy);
            frame.view()->paint(context, rect);
            context.restore();
        }

        SkPixmap pixels;
        if (!surface->peekPixels(&pixels) || pixels.rowBytes() != static_cast<size_t>(width) * 4) {
            broken = true;
            return;
        }
        const auto *data = static_cast<const uint8_t *>(pixels.addr());
        size_t size = static_cast<size_t>(width) * height * 4;
        bool retained = !nativeEmbedding.enabled && oldWidth == width && oldHeight == height &&
                        previous.size() == size;
        std::vector<ModernOS::ViewState::ScrollCopy> copies;
        std::vector<IntRect> damage;
        if (!retained) {
            damage.emplace_back(0, 0, width, height);
        } else {
            if (!repaintAll) {
                copies = scrollCopies;
                // Replay the exact moves used in the raster backing store.
                // The retained mirror starts with identical pixels; painting
                // is clipped to repaint, so only those pixels need comparison.
                for (const auto &copy : copies) {
                    const auto &rect = copy.destination;
                    browser_packet packet = {};
                    packet.magic = BROWSER_MAGIC;
                    packet.type = BROWSER_COPY;
                    packet.x = rect.x();
                    packet.y = rect.y();
                    packet.width = rect.width();
                    packet.height = rect.height();
                    packet.canvas_width = width;
                    packet.canvas_height = height;
                    packet.bytes = sizeof(browser_copy_source);
                    browser_copy_source source = {copy.source.x(), copy.source.y()};
                    CHECK(browser_surface_copy(reinterpret_cast<uint32_t*>(previous.data()),width,height,&packet,&source));
                }
            }

            // Keep separate invalidated regions separate in the transport.
            // A fixed header and exposed bottom strip must not create a
            // full-height damage rectangle containing unchanged pixels.
            for (const auto &invalid : repaint.rects())
                for (int column = invalid.x(); column < invalid.maxX(); column += 64) {
                    int endColumn = std::min(column + 64, invalid.maxX());
                    int left = endColumn, top = invalid.maxY(), right = column,
                        bottom = invalid.y();
                    for (int y = invalid.y(); y < invalid.maxY(); y++) {
                        const uint32_t *a = reinterpret_cast<const uint32_t *>(data) +
                                            static_cast<size_t>(y) * width;
                        const uint32_t *b = reinterpret_cast<const uint32_t *>(previous.data()) +
                                            static_cast<size_t>(y) * width;
                        if (!memcmp(a + column, b + column,
                                    static_cast<size_t>(endColumn - column) * 4))
                            continue;
                        int x = column, end = endColumn;
                        while (x < end && a[x] == b[x])
                            ++x;
                        while (end > x && a[end - 1] == b[end - 1])
                            --end;
                        left = std::min(left, x);
                        right = std::max(right, end);
                        top = std::min(top, y);
                        bottom = y + 1;
                    }

                    if (right <= left || bottom <= top)
                        continue;
                    IntRect rect(left, top, right - left, bottom - top);
                    if (!damage.empty() && damage.back().maxX() == left &&
                        damage.back().y() == top && damage.back().height() == rect.height())
                        damage.back().unite(rect);
                    else
                        damage.push_back(rect);
                }
        }

        if (damage.size() + copies.size() > BROWSER_PAINT_MAX_PACKETS) {
            copies.clear();
            damage = {IntRect(0, 0, width, height)};
        }
        if (damage.empty() && copies.empty())
            return;
        size_t bytes = sizeof(browser_packet) +
                       copies.size() * (sizeof(browser_packet) + sizeof(browser_copy_source));
        for (const auto &rect : damage)
            bytes += sizeof(browser_packet) + static_cast<size_t>(rect.width()) * rect.height() * 4;
        std::vector<uint8_t> current(bytes);
        size_t offset = 0;
        for (const auto &copy : copies) {
            const auto &rect = copy.destination;
            browser_packet packet = {};
            packet.magic = BROWSER_MAGIC;
            packet.type = BROWSER_COPY;
            packet.x = rect.x();
            packet.y = rect.y();
            packet.width = rect.width();
            packet.height = rect.height();
            packet.canvas_width = width;
            packet.canvas_height = height;
            packet.bytes = sizeof(browser_copy_source);
            browser_copy_source source = {copy.source.x(), copy.source.y()};
            memcpy(current.data() + offset, &packet, sizeof(packet));
            offset += sizeof(packet);
            memcpy(current.data() + offset, &source, sizeof(source));
            offset += sizeof(source);
        }

        for (const auto &rect : damage) {
            browser_packet packet = {};
            packet.magic = BROWSER_MAGIC;
            packet.type = BROWSER_FRAME;
            packet.x = rect.x();
            packet.y = rect.y();
            packet.width = rect.width();
            packet.height = rect.height();
            packet.canvas_width = width;
            packet.canvas_height = height;
            packet.bytes = packet.width * packet.height * 4;
            memcpy(current.data() + offset, &packet, sizeof(packet));
            offset += sizeof(packet);
            for (int y = rect.y(); y < rect.maxY(); y++)
                memcpy(current.data() + offset +
                           static_cast<size_t>(y - rect.y()) * packet.width * 4,
                       data + (static_cast<size_t>(y) * width + rect.x()) * 4,
                       static_cast<size_t>(packet.width) * 4);
            offset += packet.bytes;
        }

        // The desktop can observe completion only after every copy and damage
        // rectangle in this actual paint has arrived, including large frames.
        browser_packet present = {};
        present.magic = BROWSER_MAGIC;
        present.type = BROWSER_PRESENT;
        present.canvas_width = width;
        present.canvas_height = height;
        memcpy(current.data() + offset, &present, sizeof(present));
        if (!retained)
            previous.assign(data, data + size);
        else
            for (const auto &rect : damage)
                for (int y = rect.y(); y < rect.maxY(); ++y)
                    memcpy(previous.data() + (static_cast<size_t>(y) * width + rect.x()) * 4,
                           data + (static_cast<size_t>(y) * width + rect.x()) * 4,
                           static_cast<size_t>(rect.width()) * 4);
        oldWidth = width;
        oldHeight = height;
        queue(std::move(current));
    }
};

static NativeOutput *nativeOutput;
static unsigned nativeState;
static bool nativeQuit;
static ModernOS::BrowserHistory *nativeHistory;
static std::deque<std::pair<browser_packet, std::string>> nativeInput;

static void nativeText(unsigned type, const String &value) {
    nativeOutput->text(type, value);
}

static void nativeReportURL(const URL &origin, const String &url, int token, unsigned flags = 0) {
    if (url.isEmpty())
        return;
    nativeText(BROWSER_URL, origin.host() == "modernos.invalid"_s ? "about:blank"_s : url);
    browser_packet packet = {};
    packet.type = BROWSER_URL;
    packet.x = token;
    packet.flags = flags;
    auto bytes = url.utf8();
    if (browser_control_send(0, packet, bytes.data()) < 0)
        nativeQuit = true;
}

static void nativeLoading(bool loading) {
    nativeState = (nativeState & ~BROWSER_LOADING) | (loading ? BROWSER_LOADING : 0);
    nativeOutput->text(BROWSER_STATE, {}, nativeState);
}

static void nativeNavigation(const ResourceRequest &request) {
    if (request.httpMethod() != "GET"_s) {
        nativeText(BROWSER_NOTICE, "Cross-origin form submissions are not available yet."_s);
        return;
    }
    auto url = request.url().string().utf8();
    browser_packet packet = {};
    packet.type = BROWSER_NAVIGATE;
    if (browser_control_send(0, packet, url.data()) < 0)
        nativeQuit = true;
}

static void nativeHistoryTraversal(int delta) {
    if (!delta || delta < -128 || delta > 128)
        return;
    browser_packet packet = {};
    packet.type = delta < 0 ? BROWSER_BACK : BROWSER_FORWARD;
    packet.x = std::abs(delta);
    if (browser_control_send(0, packet, nullptr) < 0)
        nativeQuit = true;
}

static void nativeSwapToRemote(Ref<LocalFrame>, ResourceRequest);
static void nativeFrameCommitted(LocalFrame &, unsigned);
static void nativeNotifyActivation(LocalFrame &, MonotonicTime);
static void nativeConsumeActivation();
static void nativePublishOwnedTree();
class NativeFrameClient final : public EmptyFrameLoaderClient {
    WTF_MAKE_TZONE_ALLOCATED(NativeFrameClient);

  public:
    NativeFrameClient(LocalFrame &frame, FrameLoader &loader, URL origin)
        : EmptyFrameLoaderClient(loader), m_frame(frame), m_origin(WTF::move(origin)) {
    }

  private:
    LocalFrame &m_frame;
    URL m_origin;

    bool hasWebView() const override {
        return true;
    }

    bool canCachePage() const override {

        os_memory_usage memory = {};
        return !syscall(MODERNOS_MEMORY_USAGE, &memory) &&
               memory.mapped_bytes < 256ULL * 1024 * 1024;
    }

    void didRestoreFromBackForwardCache() override {
        if (m_frame.isMainFrame() && m_frame.view())
            m_frame.view()->resize(viewState.width, viewState.height);
        viewState.invalidateAll();
    }

    RefPtr<LocalFrame> createFrame(const AtomString &name, HTMLFrameOwnerElement &owner) override {
        if (!m_frame.page())
            return nullptr;
        auto flags = owner.sandboxFlags();
        flags.add(m_frame.effectiveSandboxFlags());
        auto policy = owner.referrerPolicy();
        if (policy == ReferrerPolicy::EmptyString)
            policy = owner.document().referrerPolicy();
        Ref child = LocalFrame::createSubframe(
            *m_frame.page(),
            [origin = m_origin](LocalFrame &frame,
                                FrameLoader &loader) -> UniqueRef<LocalFrameLoaderClient> {
                return makeUniqueRefWithoutRefCountedCheck<NativeFrameClient>(frame, loader,
                                                                              origin);
            },
            generateFrameIdentifier(), flags, policy, owner, FrameTreeSyncData::create());
        child->tree().setSpecifiedName(name);
        child->init();
        nativePublishOwnedTree();
        if (!child->page())
            return nullptr;
        return child.ptr();
    }

    bool isEmptyFrameLoaderClient() const override {
        return false;
    }

    RefPtr<HistoryItem> createHistoryItemTree(bool clipAtTarget,
                                              BackForwardItemIdentifier id) const override {
        return m_frame.loader().history().createItemTree(m_frame, clipAtTarget, id);
    }

    void dispatchDidReachLayoutMilestone(OptionSet<LayoutMilestone>) override {
        viewState.invalidateAll();
    }

    ShouldGoToHistoryItem shouldGoToHistoryItem(HistoryItem &item,
                                                IsSameDocumentNavigation) const override {
        return canHandleRequest(ResourceRequest(item.url())) ? ShouldGoToHistoryItem::Yes
                                                             : ShouldGoToHistoryItem::No;
    }

    void dispatchGoToBackForwardItemAtIndex(int steps) override {
        nativeHistoryTraversal(steps);
    }

    void dispatchEnqueueHistoryTraversalDelta(int steps) override {
        RunLoop::mainSingleton().dispatch([steps] { nativeHistoryTraversal(steps); });
    }

    bool canHandleRequest(const ResourceRequest &request) const override {
        return (!m_frame.isMainFrame() &&
                (request.url().protocolIsAbout() || request.url().protocolIsData())) ||
               (request.url().protocolIsInHTTPFamily() &&
                SecurityOrigin::create(request.url())
                    ->isSameOriginAs(SecurityOrigin::create(m_origin)));
    }

    bool canShowMIMEType(const String &mime) const override {
        return mime == "text/html"_s || mime == "application/xhtml+xml"_s ||
               mime == "text/plain"_s || mime.startsWith("image/"_s);
    }

    String userAgent(const URL &) const override {
        // Use WebKit's maintained compatibility tokens, retaining our own
        // application identity. An incomplete hand-written UA gets rejected.
        return standardUserAgent("ModernOS"_s, "1.0"_s);
    }

    void committedLoad(DocumentLoader *loader, const SharedBuffer &bytes) override {
        loader->commitData(bytes);
    }

    void transitionToCommittedForNewPage(InitializingIframe) override {
        m_frame.createView({viewState.width, viewState.height}, Color::white, {}, false,
                           ScrollbarMode::Auto, false, ScrollbarMode::Auto, false);
        viewState.invalidateAll();
    }

    void dispatchDecidePolicyForResponse(const ResourceResponse &response, const ResourceRequest &,
                                         const String &, FramePolicyFunction &&done) override {
        if (!canShowMIMEType(response.mimeType()))
            nativeText(BROWSER_NOTICE, "This document type is not supported by the browser."_s);
        done(canShowMIMEType(response.mimeType()) ? PolicyAction::Use : PolicyAction::Ignore);
    }

    void dispatchDecidePolicyForNavigationAction(
        const NavigationAction &, const ResourceRequest &request, const ResourceResponse &,
        FormState *, const String &, std::optional<NavigationIdentifier>,
        std::optional<HitTestResult> &&, bool, NavigationUpgradeToHTTPSBehavior, SandboxFlags,
        PolicyDecisionMode, FramePolicyFunction &&done) override {
        bool allowed = canHandleRequest(request);
        if (!allowed && request.url().protocolIsInHTTPFamily()) {
            if (m_frame.isMainFrame() || (nativeEmbedding.enabled && m_frame.isRootFrame()))
                nativeNavigation(request);
            else
                RunLoop::mainSingleton().dispatch(
                    [frame = Ref{m_frame}, request = ResourceRequest(request)]() mutable {
                        nativeSwapToRemote(WTF::move(frame), WTF::move(request));
                    });
        }

        done(allowed ? PolicyAction::Use : PolicyAction::Ignore);
    }

    void dispatchDidStartProvisionalLoad() override {
        if (m_frame.isMainFrame())
            nativeLoading(true);
    }

    void dispatchDidReceiveTitle(const StringWithDirection &title) override {
        if (m_frame.isMainFrame())
            nativeText(BROWSER_TITLE, title.string);
    }

    void reportURL(unsigned flags = 0) {
        if (!m_frame.isMainFrame())
            return;
        nativeReportURL(m_origin, m_frame.document()->url().string(), nativeHistory->token(),
                        flags | (nativeHistory->takeTraversal() ? BROWSER_HISTORY_TRAVERSE : 0));
        nativeText(BROWSER_TITLE, m_frame.document()->title());
    }

    void dispatchDidCommitLoad(std::optional<HasInsecureContent>, std::optional<UsedLegacyTLS>,
                               std::optional<WasPrivateRelayed>) override {
        nativeFrameCommitted(m_frame, false);
    }

    void didNotifyUserActivation(MonotonicTime time) override {
        nativeNotifyActivation(m_frame, time);
    }

    void didConsumeUserActivation() override {
        nativeConsumeActivation();
    }

    void dispatchLoadEventToOwnerElementInAnotherProcess() override {
        nativeFrameCommitted(m_frame, 2);
    }

    void dispatchDidChangeLocationWithinPage() override {
        reportURL();
    }

    void dispatchDidPushStateWithinPage() override {
        dispatchDidChangeLocationWithinPage();
    }

    void dispatchDidReplaceStateWithinPage() override {
        reportURL(BROWSER_HISTORY_REPLACE);
    }

    void dispatchDidFinishLoad() override {
        if (m_frame.isMainFrame()) {
            reportURL();
            nativeLoading(false);
        }
        nativeFrameCommitted(m_frame, true);
        viewState.invalidateAll();
    }

    void dispatchDidFailProvisionalLoad(const ResourceError &error, WillContinueLoading,
                                        WillInternallyHandleFailure) override {
        if (m_frame.isMainFrame()) {
            if (!error.isCancellation())
                nativeText(BROWSER_NOTICE, error.localizedDescription());
            nativeLoading(false);
        }
    }

    void dispatchDidFailLoad(const ResourceError &error) override {
        if (m_frame.isMainFrame()) {
            if (!error.isCancellation())
                nativeText(BROWSER_NOTICE, error.localizedDescription());
            nativeLoading(false);
        }
    }
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(NativeFrameClient);
#include "RemoteFrames.inc"

class NativeMouseEvent final : public PlatformMouseEvent {
  public:
    NativeMouseEvent(Type type, MouseButton button, unsigned short buttons, IntPoint point,
                     IntPoint screenPoint, OptionSet<Modifier> modifiers, int clicks = 0)
        : PlatformMouseEvent(point, screenPoint, button, type, clicks, modifiers,
                             MonotonicTime::now(), 0, SyntheticClickType::NoTap,
                             MouseEventInputSource::UserDriven) {
        m_buttons = buttons;
    }
};

static OptionSet<PlatformEvent::Modifier> nativeModifiers;
static unsigned nativeButtons;

static void nativeKey(LocalFrame &frame, int code, bool down) {
    if (nativeFocusedRemote && nativeRemoteFrames.contains(nativeFocusedRemote)) {
        nativeFrameSend({.type = WEB_FRAME_INPUT,
                         .flags = down ? 1U : 0U,
                         .frame = nativeFocusedRemote,
                         .x = code,
                         .height = 2});
        return;
    }

    using M = PlatformEvent::Modifier;
    if (code == 304 || code == 306 || code == 308) {
        auto mod = code == 304 ? M::ShiftKey : code == 306 ? M::ControlKey : M::AltKey;
        if (down)
            nativeModifiers.add(mod);
        else
            nativeModifiers.remove(mod);
        return;
    }

    String text, domKey, domCode;
    int virtualKey = code;
    if (code >= BROWSER_TEXT_KEY_BASE && code < BROWSER_TEXT_KEY_BASE + 128) {
        char c = code - BROWSER_TEXT_KEY_BASE;
        text = String::fromUTF8(std::span{&c, 1});
        domKey = text;
        virtualKey = toupper(c);
    } else {
        switch (code) {
        case 13:
            text = "\r"_s;
            domKey = "Enter"_s;
            break;
        case 8:
            text = "\b"_s;
            domKey = "Backspace"_s;
            break;
        case 9:
            text = "\t"_s;
            domKey = "Tab"_s;
            break;
        case 27:
            domKey = "Escape"_s;
            break;
        case 127:
            virtualKey = 46;
            domKey = "Delete"_s;
            break;
        case 273:
            virtualKey = 38;
            domKey = "ArrowUp"_s;
            break;
        case 274:
            virtualKey = 40;
            domKey = "ArrowDown"_s;
            break;
        case 275:
            virtualKey = 39;
            domKey = "ArrowRight"_s;
            break;
        case 276:
            virtualKey = 37;
            domKey = "ArrowLeft"_s;
            break;
        case 278:
            virtualKey = 36;
            domKey = "Home"_s;
            break;
        case 279:
            virtualKey = 35;
            domKey = "End"_s;
            break;
        case 280:
            virtualKey = 33;
            domKey = "PageUp"_s;
            break;
        case 281:
            virtualKey = 34;
            domKey = "PageDown"_s;
            break;
        default:
            if (code >= 32 && code < 127) {
                char c = code;
                domKey = String::fromUTF8(std::span{&c, 1});
                virtualKey = toupper(c);
                if (!nativeModifiers.contains(M::ControlKey))
                    text = domKey;
            } else
                return;
        }
    }

    PlatformKeyboardEvent event(down ? PlatformEvent::Type::KeyDown : PlatformEvent::Type::KeyUp,
                                down ? text : String(), down ? text : String(), domKey, domCode, {},
                                virtualKey, false, false, false, nativeModifiers,
                                MonotonicTime::now());
    RefPtr focused = frame.page()->focusController().focusedOrMainFrame();
    (focused ? *focused : frame).eventHandler().keyEvent(event);
    viewState.dirty = true;
}

static void nativeEvent(LocalFrame &frame, const browser_packet &packet, const char *text) {
    switch (packet.type) {
    case BROWSER_QUIT:
        nativeQuit = true;
        break;
    case BROWSER_STATE:
        nativeState = (nativeState & BROWSER_LOADING) |
                      (packet.flags & (BROWSER_CAN_BACK | BROWSER_CAN_FORWARD));
        nativeOutput->text(BROWSER_STATE, {}, nativeState);
        break;
    case BROWSER_SCREEN:
        if (!ModernOS::setScreenSize(packet.width, packet.height, packet.flags) ||
            !ModernOS::setWindowGeometry(packet.x, packet.y, packet.canvas_width,
                                         packet.canvas_height))
            nativeQuit = true;
        break;
    case BROWSER_RESIZE:
        if (packet.width < (nativeEmbedding.enabled ? 1 : 300) ||
            packet.height < (nativeEmbedding.enabled ? 1 : 200) || packet.width > 4096 ||
            packet.height > 4096 ||
            static_cast<uint64_t>(packet.width) * packet.height > BROWSER_MAX_PIXELS) {
            nativeQuit = true;
            break;
        }

        viewState.width = packet.width;
        viewState.height = packet.height;
        if (frame.view())
            frame.view()->resize(packet.width, packet.height);
        viewState.invalidateAll();
        break;
    case BROWSER_NAVIGATE: {
        URL url{String::fromUTF8(text)};
        if (!url.isValid())
            break;
        FrameLoadRequest request(frame, ResourceRequest(WTF::move(url)));
        if (packet.flags & BROWSER_NAVIGATION_FROM_CLIENT)
            request.setIsRequestFromClientOrUserInput();
        frame.loader().load(WTF::move(request));
        break;
    }
    case BROWSER_RELOAD:
        frame.loader().reload({}, true);
        break;
    case BROWSER_BACK:
    case BROWSER_FORWARD:
        if (auto item = nativeHistory->findToken(packet.x))
            frame.page()->goToItem(frame, *item, FrameLoadType::IndexedBackForward,
                                   ShouldTreatAsContinuingLoad::No);
        break;
    case BROWSER_STOP:
        frame.loader().stopAllLoaders();
        nativeLoading(false);
        break;
    case BROWSER_KEY:
        nativeKey(frame, packet.x, !!packet.flags);
        break;
    case BROWSER_POINTER: {
        IntPoint point{std::clamp(packet.x, 0, viewState.width - 1),
                       std::clamp(packet.y, 0, viewState.height - 1)};
        IntPoint screenPoint{static_cast<int>(packet.canvas_width),
                             static_cast<int>(packet.canvas_height)};
        unsigned buttons = packet.flags & 7;
        auto pointer = packet;
        pointer.width = 0;
        bool changed = buttons != nativeButtons || packet.width;
        for (unsigned bit = 0; bit < 3; bit++) {
            unsigned mask = 1u << bit;
            if (!((buttons ^ nativeButtons) & mask))
                continue;
            MouseButton button = bit == 0   ? MouseButton::Left
                                 : bit == 1 ? MouseButton::Middle
                                            : MouseButton::Right;
            NativeMouseEvent event(buttons & mask ? PlatformEvent::Type::MousePressed
                                                  : PlatformEvent::Type::MouseReleased,
                                   button, buttons, point, screenPoint, nativeModifiers, 1);
            auto result = buttons & mask ? frame.eventHandler().handleMousePressEvent(event)
                                         : frame.eventHandler().handleMouseReleaseEvent(event);
            if (auto remote = result.remoteUserInputEventData())
                nativeRemotePointer(*remote, pointer);
            else if (buttons & mask)
                nativeFocusedRemote = 0;
        }

        NativeMouseEvent move(PlatformEvent::Type::MouseMoved, MouseButton::None, buttons, point,
                              screenPoint, nativeModifiers);
        auto moved = frame.eventHandler().mouseMoved(move);
        if (auto remote = moved.remoteUserInputEventData())
            nativeRemotePointer(*remote, pointer);
        nativeButtons = buttons;
        if (packet.width) {
            float delta = std::clamp(packet.width, -32, 32) * 100.f;
            PlatformWheelEvent event(
                point, screenPoint, 0, delta, 0, static_cast<float>(packet.width),
                PlatformWheelEventGranularity::ScrollByPixelWheelEvent,
                nativeModifiers.contains(PlatformEvent::Modifier::ShiftKey),
                nativeModifiers.contains(PlatformEvent::Modifier::ControlKey),
                nativeModifiers.contains(PlatformEvent::Modifier::AltKey), false);
            auto result = frame.eventHandler().handleWheelEvent(
                event, {WheelEventProcessingSteps::BlockingDOMEventDispatch,
                        WheelEventProcessingSteps::SynchronousScrolling});
            if (auto remote = result.first.remoteUserInputEventData())
                nativeRemotePointer(*remote, packet);
        }

        if (changed)
            viewState.dirty = true;
        break;
    }
    default:
        nativeQuit = true;
        break;
    }
}

static int nativeRun(const char *target, int width, int height) {
    Process::setIdentifier(ProcessIdentifier(static_cast<uint64_t>(getpid())));
    os_web_identity identity = {};
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&identity)==0 && identity.role==OS_WEB_RENDERER);
    URL origin{String::fromUTF8(identity.origin)}, url{String::fromUTF8(target)};
    bool start = !strcmp(target, "about:blank");
    if (start)
        url = URL(origin, "/"_s);
    CHECK(width>=(nativeEmbedding.enabled?1:300) && height>=(nativeEmbedding.enabled?1:200) && width<=4096 && height<=4096 && static_cast<uint64_t>(width)*height<=BROWSER_MAX_PIXELS);
    CHECK(url.isValid() && SecurityOrigin::create(url)->isSameOriginAs(SecurityOrigin::create(origin)));
    CHECK(write(0,&identity,sizeof(identity))==(ssize_t)sizeof(identity));

    os_memory_usage memory = {};
    CHECK(!syscall(MODERNOS_MEMORY_USAGE,&memory) && memory.limit_bytes);
    char memoryLimit[32];
    snprintf(memoryLimit, sizeof(memoryLimit), "%llu",
             static_cast<unsigned long long>(memory.limit_bytes));
    CHECK(!setenv("JSC_forceRAMSize",memoryLimit,1));
    CHECK(!setenv("JSC_gcMaxHeapSize","33554432",1));
    // This VM eagerly backs reservations. Avoid allocating hundreds of MiB
    // for WebKit's desktop StructureID address-space reservation at startup.
    CHECK(!setenv("JSC_structureHeapSizeInKB","65536",1));
#if ENABLE(WEBASSEMBLY)
    // IPInt performs actual bounds checks. Large signal-based reservations
    // would consume physical RAM in this kernel's eager mapping model.
    CHECK(!setenv("JSC_useWasmFastMemory","false",1));
    CHECK(!setenv("JSC_useWasmFaultSignalHandler","false",1));
    CHECK(!setenv("JSC_useWasmIPInt","true",1));
#endif

    ScriptController::initializeMainThread();
    ModernOS::registerMediaEngine();
    auto &pressure = MemoryPressureHandler::singleton();
    pressure.setConfiguration(
        MemoryPressureHandler::Configuration(memory.limit_bytes, .65, .80, std::nullopt, 2_s));
    pressure.setLowMemoryHandler([](Critical critical, Synchronous synchronous) {
        WebCore::releaseMemory(critical, synchronous);
    });
    pressure.setShouldUsePeriodicMemoryMonitor(true);
    pressure.install();

    NativeOutput output;
    nativeOutput = &output;
    viewState.width = width;
    viewState.height = height;

    ModernOS::BrokerStrategies strategies(URL(origin),
                                          [](const browser_packet &packet, const char *text) {
                                              if (nativeInput.size() >= 256) {
                                                  nativeQuit = true;
                                                  return;
                                              }
                                              nativeInput.emplace_back(packet, text);
                                          });
    setPlatformStrategies(&strategies);
    strategies.brokerLoader().setFrameControl(
        [](const web_frame_packet &packet, std::string text, RefPtr<SharedBuffer> bytes) {
            if (nativeFrameInputs.size() >= 32) {
                nativeQuit = true;
                return;
            }
            nativeFrameInputs.push_back({packet, std::move(text), WTF::move(bytes)});
        });

    ModernOS::setAudioSink([&strategies, origin](unsigned operation,
                                                 std::span<const int16_t> samples) {
        unsigned service = operation == OS_AUDIO_WRITE  ? WEB_SERVICE_AUDIO_WRITE
                           : operation == OS_AUDIO_INFO ? WEB_SERVICE_AUDIO_INFO
                                                        : WEB_SERVICE_AUDIO_RESET;
        return static_cast<int>(
            strategies.brokerLoader()
                .service(service, origin, {},
                         {reinterpret_cast<const uint8_t *>(samples.data()), samples.size_bytes()})
                .status);
    });

    ModernOS::setAudioQueue([&strategies, origin]() -> std::optional<uint64_t> {
        auto result = strategies.brokerLoader().service(WEB_SERVICE_AUDIO_INFO, origin);
        os_audio_info info{};
        if (result.status || result.bytes.size() != sizeof(info))
            return std::nullopt;
        memcpy(&info, result.bytes.span().data(), sizeof(info));
        if (info.version != 1 || info.rate != 48000 || info.channels != 2 || info.sample_bits != 16)
            return std::nullopt;
        return info.queued_frames;
    });

    CHECK(modernOSFonts());
    // Keep scrollbars visible in the CPU desktop. Overlay fades otherwise
    // schedule repeated paints after the content has finished scrolling.
    auto platformSettings = SystemSettings::singleton().settingsState();
    platformSettings.overlayScrolling = false;
    SystemSettings::singleton().updateSettings(platformSettings);

    // A real page identifier is required by WebCore's media session manager.
    // Anonymous component fixtures deliberately have no playback session.
    auto configuration = pageConfigurationWithEmptyClients(PageIdentifier::generate(),
                                                           PAL::SessionID::defaultSessionID());
    Ref history = adoptRef(
        *new ModernOS::BrowserHistory([origin](const String &url, int token, bool traversal) {
            nativeReportURL(origin, url, token, traversal ? BROWSER_HISTORY_TRAVERSE : 0);
        }));
    nativeHistory = history.ptr();
    configuration.backForwardClient = history.copyRef();
    configuration.editorClient = makeUniqueRef<ModernOS::EditorModernOS>();
    configuration.chromeClient = makeUniqueRef<ModernOS::ChromeModernOS>(viewState);
    configuration.cookieJar = adoptRef(*new ModernOS::BrokerCookieJar(strategies.brokerLoader()));
    configuration.storageNamespaceProvider =
        adoptRef(*new ModernOS::BrokerStorageProvider(strategies.brokerLoader()));
    configuration.mainFrameCreationParameters = PageConfiguration::LocalMainFrameCreationParameters{
        [origin](LocalFrame &frame, FrameLoader &loader) -> UniqueRef<LocalFrameLoaderClient> {
            return makeUniqueRefWithoutRefCountedCheck<NativeFrameClient>(frame, loader, origin);
        },
        {},
        ReferrerPolicy::EmptyString};

    std::vector<NativeTreeEntry> embeddingTree;
    if (nativeEmbedding.enabled) {
        auto snapshot = strategies.brokerLoader().service(WEB_SERVICE_FRAME_TREE, url);
        CHECK(!snapshot.status && (snapshot.flags&WEB_SERVICE_FOUND) && nativeDecodeTree(snapshot.bytes.span(),embeddingTree));
        CHECK(!embeddingTree.empty());
        configuration.mainFrameIdentifier = FrameIdentifier(embeddingTree.front().frame);
        configuration.mainFrameCreationParameters =
            CompletionHandler<UniqueRef<RemoteFrameClient>(RemoteFrame &)>{
                [](RemoteFrame &frame) -> UniqueRef<RemoteFrameClient> {
                    return makeUniqueRef<NativeRemoteClient>(frame, true);
                }};
    }

    RefPtr page = Page::create(WTF::move(configuration));
    activePage = page.get();
    std::unique_ptr<NativeExceptionDebugger> exceptionDebugger;
    if (nativeExceptionTrace) {
        exceptionDebugger = std::make_unique<NativeExceptionDebugger>(commonVM());
        page->setDebugger(exceptionDebugger.get());
    }
    auto detachExceptionDebugger = makeScopeExit([&] {
        if (exceptionDebugger && page)
            page->setDebugger(nullptr);
    });

    BackForwardCache::singleton().setMaxSize(1);
    auto &settings = page->settings();
    settings.setScriptEnabled(true);
    settings.setImagesEnabled(true);
    settings.setLoadsImagesAutomatically(true);
    // Keep upstream's default for this experimental API. Sites use their
    // timer fallback; enabling it delays initialization on script-heavy pages.
    settings.setRequestIdleCallbackEnabled(false);
    settings.setAcceleratedCompositingEnabled(false);
    settings.setLocalStorageEnabled(true);
    settings.setSessionStorageEnabled(true);
    // WebCore animates the actual scrollable area after DOM wheel dispatch,
    // retaining nested scrolling, scroll-snap and preventDefault semantics.
    // Retained raster moves and exposed-strip damage bound the pixel traffic.
    settings.setScrollAnimatorEnabled(true);
    // The native decoder supplies actual frame timestamps and presentation
    // counts, allowing WebCore to deliver real video-frame callbacks.
    settings.setRequestVideoFrameCallbackEnabled(true);
    settings.setSiteIsolationEnabled(true);
    settings.setUserActivationAPIEnabled(true);
    settings.setIndexedDBAPIEnabled(false);
    settings.setServiceWorkersEnabled(false);
    settings.setStandardFontFamily(AtomString("DejaVu Sans"_s));
    settings.setCookieStoreAPIEnabled(false);
    settings.setCookieStoreManagerEnabled(false);
    settings.setUsesBackForwardCache(true);
    settings.setSansSerifFontFamily(AtomString("DejaVu Sans"_s));
    settings.setSerifFontFamily(AtomString("DejaVu Serif"_s));
    settings.setFixedFontFamily(AtomString("DejaVu Sans Mono"_s));
    settings.setDefaultFontSize(16);
    settings.setDefaultFixedFontSize(13);
    RefPtr<LocalFrame> frame = page->localMainFrame();
    if (nativeEmbedding.enabled) {
        // FrameTreeSyncData describes the immediate parent. WebCore's Page
        // separately owns the top document origin used by workers and storage.
        // The broker supplies it from the real frame graph, including nested
        // frames and HTTP ancestors; leaving it unset gives workers an opaque
        // top origin and incorrectly hides secure-context APIs on HTTPS pages.
        page->setMainFrameURLAndOrigin(nativeEmbedding.topOrigin, nullptr);
        CHECK(nativeApplyTree(*page,embeddingTree,0,true));
        const NativeTreeEntry *rootEntry = nullptr;
        for (const auto &entry : embeddingTree)
            if (entry.frame == nativeEmbedding.frame && entry.local)
                rootEntry = &entry;
        CHECK(rootEntry && rootEntry->parent);
        RefPtr parent = nativeFindFrame(*page, rootEntry->parent);
        CHECK(parent && is<RemoteFrame>(*parent));
        frame = LocalFrame::createProvisionalSubframe(
            *page,
            [origin](LocalFrame &frame, FrameLoader &loader) -> UniqueRef<LocalFrameLoaderClient> {
                return makeUniqueRefWithoutRefCountedCheck<NativeFrameClient>(frame, loader,
                                                                              origin);
            },
            FrameIdentifier(nativeEmbedding.frame), SandboxFlags::fromRaw(nativeEmbedding.sandbox),
            ReferrerPolicy::StrictOriginWhenCrossOrigin, ScrollbarMode::Auto, *parent,
            FrameTreeSyncData::create());
        parent->tree().appendChild(*frame);
        frame->tree().setSpecifiedName(AtomString(rootEntry->name));
        CHECK(nativeApplyTree(*page,embeddingTree,0,true));
    }

    CHECK(frame);
    nativeRootID = frame->frameID().toRawValue();
    frame->init();
    frame->createView({width, height}, Color::white, {});
    viewState.focusedFrameChanged = nativeFocusChanged;
    auto clearFocusCallback = makeScopeExit([&] { viewState.focusedFrameChanged = {}; });
    nativeFrameSend({.type = WEB_FRAME_HELLO, .frame = nativeRootID});
    nativeTreeRoot = frame.get();
    nativePublishOwnedTree();
    auto clearTreeRoot = makeScopeExit([&] {
        nativeTreeRoot = nullptr;
        nativeTreeMirrors.clear();
    });
    page->setIsInWindow(true);
    page->setActivityState({ActivityState::IsVisible, ActivityState::IsInWindow,
                            ActivityState::IsFocused, ActivityState::WindowIsActive});
    CHECK(fcntl(1,F_SETFL,fcntl(1,F_GETFL)|O_NONBLOCK)==0);
    output.start();
    nativeText(BROWSER_URL, start ? "about:blank"_s : url.string());
    nativeLoading(true);
    if (start) {
        static constexpr char html[] =
            "<!doctype html><meta charset=utf-8><title>ModernOS Browser</title><style>body{margin:72px;font:18px sans-serif;color:#283545;background:#f5f7fa}h1{font-size:36px}p{max-width:640px;line-height:1.6}a{color:#116899}</style><h1>Browse the web</h1><p>Enter a website or search in the address bar.</p><p><a href='https://www.google.com/'>Google</a> &nbsp; <a href='https://osdev.org/'>OSDev</a> &nbsp; <a href='https://www.youtube.com/'>YouTube</a></p>";
        auto buffer = SharedBuffer::create(
            std::span{reinterpret_cast<const uint8_t *>(html), sizeof(html) - 1});
        ResourceResponse response(URL(url), "text/html"_s, sizeof(html) - 1, "UTF-8"_s);
        SubstituteData data(WTF::move(buffer), {}, WTF::move(response),
                            SessionHistoryVisibility::Visible);
        frame->loader().load(FrameLoadRequest(*frame, ResourceRequest(URL(url)), WTF::move(data)));
    } else {
        ResourceRequest request{URL(url)};
        if (nativeEmbedding.enabled)
            request.setHTTPReferrer(nativeEmbedding.referrer);
        auto saved = strategies.brokerLoader().service(WEB_SERVICE_HISTORY_REQUEST, url);
        if (!saved.status && (saved.flags & WEB_SERVICE_FOUND) && saved.bytes.size() >= 8) {
            uint32_t lengths[2];
            memcpy(lengths, saved.bytes.span().data(), sizeof(lengths));
            if (!lengths[0] || lengths[0] > 16 || lengths[1] > 4096 ||
                static_cast<uint64_t>(lengths[0]) + lengths[1] + 8 > saved.bytes.size())
                return 1;
            auto data = saved.bytes.span();
            request.setHTTPMethod(String::fromUTF8(data.subspan(8, lengths[0])));
            request.setHTTPContentType(String::fromUTF8(data.subspan(8 + lengths[0], lengths[1])));
            request.setHTTPBody(FormData::create(data.subspan(8 + lengths[0] + lengths[1])));
            request.setHTTPOrigin(origin.string());
        }

        FrameLoadRequest load(*frame, WTF::move(request));
        if (nativeClientNavigation && !nativeEmbedding.enabled &&
            !(saved.flags & WEB_SERVICE_FOUND))
            load.setIsRequestFromClientOrUserInput();
        frame->loader().load(WTF::move(load));
    }

    auto lastPaint = MonotonicTime::now();
    while (!nativeQuit && !output.broken && strategies.loaderStrategy()->isOnLine()) {
        tick([] { return !nativeInput.empty() || !nativeFrameInputs.empty(); });
        nativePublishOwnedTree();
        nativeProcessFrames(*frame);
        while (!nativeInput.empty()) {
            auto event = std::move(nativeInput.front());
            nativeInput.pop_front();
            nativeEvent(*frame, event.first, event.second.c_str());
        }
        nativePublishOwnedTree();
        if (viewState.dirty && output.idle() &&
            MonotonicTime::now() - lastPaint >= page->preferredRenderingUpdateInterval()) {
            viewState.dirty = false;
            output.paint(*frame);
            lastPaint = MonotonicTime::now();
        }
    }

    frame->loader().stopAllLoaders();
    pressure.setShouldUsePeriodicMemoryMonitor(false);
    pressure.setLowMemoryHandler(nullptr);
    // Destroy media players while their broker and PCM sink are still alive.
    if (exceptionDebugger)
        page->setDebugger(nullptr);
    frame = nullptr;
    page = nullptr;
    ModernOS::setAudioSink({});
    ModernOS::setAudioQueue({});
    activePage = nullptr;
    nativeOutput = nullptr;
    nativeHistory = nullptr;
    return output.broken ? 1 : 0;
}
}
