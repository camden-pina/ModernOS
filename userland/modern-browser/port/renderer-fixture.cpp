// Real WebCore software embedder, native browser and controlled component checks.
#include "config.h"
#include "FixtureStrategies.h"
#include "BrokerLoader.h"
#include "EditorModernOS.h"
#include "ChromeModernOS.h"
#include "MediaModernOS.h"
#include "../../include/audio_protocol.h"
#include "Document.h"
#include "DocumentLoader.h"
#include "DocumentView.h"
#include "EmptyClients.h"
#include "EmptyFrameLoaderClient.h"
#include "EventHandler.h"
#include "FrameLoadRequest.h"
#include "FrameLoader.h"
#include "Font.h"
#include "FontCache.h"
#include "GarbageCollectionController.h"
#include "GraphicsContextSkia.h"
#include "HTMLInputElement.h"
#include "HandleUserInputEventResult.h"
#include "LocalFrame.h"
#include "LocalFrameInlines.h"
#include "LocalFrameView.h"
#include "Page.h"
#include "PageConfiguration.h"
#include "RunLoopObserver.h"
#include "PlatformMouseEvent.h"
#include "PlatformKeyboardEvent.h"
#include "PlatformWheelEvent.h"
#include "ScriptController.h"
#include "ScrollAnimator.h"
#include "ScrollingCoordinatorTypes.h"
#include "SecurityOrigin.h"
#include "Settings.h"
#include "SharedBuffer.h"
#include "SubstituteData.h"
#include "../../include/web_process.h"
#include "../../include/browser_protocol.h"
#include <pal/SessionID.h>
#include <fontconfig/fontconfig.h>
#include <glib.h>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkSurface.h>
#include <JavaScriptCore/SourceTaintedOrigin.h>
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/text/CString.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace WebCore;
using namespace WTF;
static constexpr int fixtureWidth = 320, fixtureHeight = 240;
static bool loadFailed;
static ModernOS::ViewState viewState;
static Page *activePage;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL renderer-fixture:%d: %s\n",__LINE__,#x); exit(1); } } while (0)

class FixtureFrameClient final : public EmptyFrameLoaderClient {
    WTF_MAKE_TZONE_ALLOCATED(FixtureFrameClient);

  public:
    FixtureFrameClient(LocalFrame &frame, FrameLoader &loader, URL url, bool network)
        : EmptyFrameLoaderClient(loader), m_frame(frame), m_url(WTF::move(url)),
          m_network(network) {
    }

  private:
    bool hasWebView() const override {
        return true;
    }

    bool isEmptyFrameLoaderClient() const override {
        return false;
    }

    bool canHandleRequest(const ResourceRequest &request) const override {
        return request.url() == m_url || (m_network && request.url().protocolIsInHTTPFamily() &&
                                          SecurityOrigin::create(request.url())
                                              ->isSameOriginAs(SecurityOrigin::create(m_url)));
    }

    bool canShowMIMEType(const String &mime) const override {
        return mime == "text/html"_s;
    }

    void committedLoad(DocumentLoader *loader, const SharedBuffer &bytes) override {
        loader->commitData(bytes);
    }

    void transitionToCommittedForNewPage(InitializingIframe) override {
        auto mode = m_network ? ScrollbarMode::Auto : ScrollbarMode::AlwaysOff;
        m_frame.createView({fixtureWidth, fixtureHeight}, Color::white, {}, false, mode, !m_network,
                           mode, !m_network);
    }

    void dispatchDecidePolicyForResponse(const ResourceResponse &response, const ResourceRequest &,
                                         const String &, FramePolicyFunction &&done) override {
        done(canShowMIMEType(response.mimeType()) ? PolicyAction::Use : PolicyAction::Ignore);
    }

    void dispatchDecidePolicyForNavigationAction(
        const NavigationAction &, const ResourceRequest &request, const ResourceResponse &,
        FormState *, const String &, std::optional<NavigationIdentifier>,
        std::optional<HitTestResult> &&, bool, NavigationUpgradeToHTTPSBehavior, SandboxFlags,
        PolicyDecisionMode, FramePolicyFunction &&done) override {
        done(canHandleRequest(request) ? PolicyAction::Use : PolicyAction::Ignore);
    }

    void dispatchDidFailProvisionalLoad(const ResourceError &error, WillContinueLoading,
                                        WillInternallyHandleFailure) override {
        loadFailed = true;
        fprintf(stderr, "WebCore provisional load failed: %s\n",
                error.localizedDescription().utf8().data());
    }

    void dispatchDidFailLoad(const ResourceError &error) override {
        loadFailed = true;
        fprintf(stderr, "WebCore load failed: %s\n", error.localizedDescription().utf8().data());
    }

    LocalFrame &m_frame;
    URL m_url;
    bool m_network;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(FixtureFrameClient);

class FixtureMouseEvent final : public PlatformMouseEvent {
  public:
    FixtureMouseEvent(Type type, unsigned short buttons, IntPoint point = {50, 120})

        : PlatformMouseEvent(point, point, MouseButton::Left, type, 1, {}, MonotonicTime::now(), 0,
                             SyntheticClickType::NoTap, MouseEventInputSource::UserDriven) {
        m_buttons = buttons;
    }
};

static void emit(LocalFrame &frame) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(fixtureWidth, fixtureHeight));
    CHECK(surface);
    surface->getCanvas()->clear(SK_ColorWHITE);
    GraphicsContextSkia context(*surface->getCanvas(), RenderingMode::Unaccelerated,
                                RenderingPurpose::Unspecified);
    frame.view()->updateLayoutAndStyleIfNeededRecursive();
    frame.view()->paint(context, {0, 0, fixtureWidth, fixtureHeight});
    SkPixmap pixels;
    CHECK(surface->peekPixels(&pixels) && pixels.rowBytes()==fixtureWidth*4);
    browser_packet header = {};
    header.magic = BROWSER_MAGIC;
    header.type = BROWSER_FRAME;
    header.width = fixtureWidth;
    header.height = fixtureHeight;
    header.bytes = fixtureWidth * fixtureHeight * 4;
    header.canvas_width = fixtureWidth;
    header.canvas_height = fixtureHeight;
    CHECK(write(1,&header,sizeof(header))==(ssize_t)sizeof(header));
    const auto *data = static_cast<const unsigned char *>(pixels.addr());
    size_t remaining = header.bytes;
    while (remaining) {
        ssize_t n = write(1, data, remaining);
        CHECK(n>0);
        data += n;
        remaining -= n;
    }
}

static void tick(bool (*inputPending)() = nullptr) {
    // GLib dispatch alone skips WebKit's before-waiting observers, including
    // opportunistic idle work. Own the context while cycling the real loop.
    auto *context = RunLoop::currentSingleton().mainContext();
    CHECK(g_main_context_acquire(context));
    unsigned cycles = 0;
    auto started = MonotonicTime::now();
    // A continuously busy page must yield between dispatches so native input
    // and prepared paints do not wait behind 32 successive script tasks.
    do {
        RunLoop::cycle();
    } while (++cycles < 32 && MonotonicTime::now() - started < 4_ms &&
             (!inputPending || !inputPending()) && g_main_context_pending(context));
    g_main_context_release(context);
    if (inputPending && inputPending())
        return;
    static MonotonicTime lastRenderingUpdate;
    auto now = MonotonicTime::now();
    if (activePage && viewState.renderingRequired &&
        now - lastRenderingUpdate >= activePage->preferredRenderingUpdateInterval()) {
        lastRenderingUpdate = now;
        viewState.renderingRequired = false;
        activePage->updateRendering();
        activePage->finalizeRenderingUpdate({});
        activePage->didUpdateRendering();
    }

    usleep(1000);
}

static void click(LocalFrame &frame, IntPoint point) {
    FixtureMouseEvent down(PlatformEvent::Type::MousePressed, 1, point),
        up(PlatformEvent::Type::MouseReleased, 0, point);
    frame.eventHandler().handleMousePressEvent(down);
    frame.eventHandler().handleMouseReleaseEvent(up);
}

static void key(LocalFrame &frame, const String &text, const String &domKey, int virtualKey) {
    PlatformKeyboardEvent down(PlatformEvent::Type::KeyDown, text, text, domKey, {}, {}, virtualKey,
                               false, false, false, {}, MonotonicTime::now());
    PlatformKeyboardEvent up(PlatformEvent::Type::KeyUp, {}, {}, domKey, {}, {}, virtualKey, false,
                             false, false, {}, MonotonicTime::now());
    frame.eventHandler().keyEvent(down);
    frame.eventHandler().keyEvent(up);
}

static void wheel(LocalFrame &frame, float delta) {
    PlatformWheelEvent event({270, 200}, {270, 200}, 0, delta, 0, delta / 120,
                             PlatformWheelEventGranularity::ScrollByPixelWheelEvent, false, false,
                             false, false);
    frame.eventHandler().handleWheelEvent(event,
                                          {WheelEventProcessingSteps::BlockingDOMEventDispatch,
                                           WheelEventProcessingSteps::SynchronousScrolling});
}

#include "renderer-native.inc"

static void checkGarbageCollection(LocalFrame &frame) {
    // Run these allocations through the selected interpreter in a real DOM
    // realm, then invoke the actual full collector. The native view has no
    // test hook or GC preference changes.
    frame.script().executeScriptIgnoringException(String::fromUTF8(R"JS(
globalThis.modernOSGCCheck = (() => {
 class Counter { #value = 0; add(value) { return this.#value += value; } }
 const counter = new Counter();
 const buffer = new ArrayBuffer(32), view = new DataView(buffer);
 view.setBigUint64(0, 0x1122334455667788n, true);
 const node = document.createElement('span');
 node.textContent = 'retained DOM wrapper';
 const retained = new Map();
 for (let i=0;i<1024;i++) retained.set(i, {i, text:'retained-'+i, node});
 const closure = () => retained.get(1023);
 return {counter, view, retained, closure, node};
})();
for (let round=0;round<16;round++) {
 const objects = Array.from({length:12000}, (_,i) => ({i, text:'garbage-'+round+'-'+i, bytes:new Uint8Array(128)}));
 if (objects[11999].text !== 'garbage-'+round+'-11999') throw Error('allocation failed');
 modernOSGCCheck.counter.add(round);
}
document.title = 'WebCore GC allocated';
)JS"),
                                                  JSC::SourceTaintedOrigin::Untainted);
    CHECK(frame.document()->title()=="WebCore GC allocated"_s);
    for (unsigned round = 0; round < 3; round++) {
        GarbageCollectionController::singleton().garbageCollectNow();
        frame.script().executeScriptIgnoringException(String::fromUTF8(R"JS(
(() => {
 const live=modernOSGCCheck;
 if(live.counter.add(1)!==120+(live.collections||0)+1 || live.retained.size!==1024 ||
    live.closure().text!=='retained-1023' || live.closure().node!==live.node ||
    live.node.textContent!=='retained DOM wrapper' ||
    live.view.getBigUint64(0,true)!==0x1122334455667788n ||
    !/\p{Letter}+/u.test('Ωλ') || new Intl.NumberFormat('en-US').format(1234.5)!=='1,234.5')
     throw Error('live interpreter object lost after collection');
 live.collections=(live.collections||0)+1;
 document.title='WebCore GC survived '+live.collections;
})();
)JS"),
                                                      JSC::SourceTaintedOrigin::Untainted);
        CHECK(frame.document()->title()==makeString("WebCore GC survived "_s,round+1));
    }
    frame.script().executeScriptIgnoringException(
        "delete globalThis.modernOSGCCheck;document.title='WebCore clicked'"_s,
        JSC::SourceTaintedOrigin::Untainted);
    fprintf(
        stderr,
        "PASS selected interpreter allocations and full GC preserve closures, private fields, DOM wrappers, Map, BigInt, typed data and ICU\n");
}

static void checkIdleCallbacks(LocalFrame &frame) {
    frame.script().executeScriptIgnoringException(String::fromUTF8(R"JS(
(()=>{
 const failed=e=>document.title='WebCore idle failed: '+e;
 let microtask=false;
 const cancelled=requestIdleCallback(()=>failed('cancelled callback'));
 cancelIdleCallback(cancelled);
 requestIdleCallback(()=>Promise.resolve().then(()=>microtask=true));
 requestIdleCallback(d=>{
  if(!microtask||d.didTimeout||d.timeRemaining()<0||d.timeRemaining()>50)return failed('idle deadline or microtask order');
  requestAnimationFrame(()=>setTimeout(()=>{
   const started=performance.now();
   requestIdleCallback(()=>{const end=performance.now()+80;while(performance.now()<end){}});
   requestIdleCallback(()=>{
    if(performance.now()-started>500)return failed('lost continuation');
    setTimeout(()=>{
     let done=false,timed=0,cancelled;
     const pulse=()=>{if(!done)window.postMessage('modernos-idle-check','*');};
     const message=e=>{if(e.data==='modernos-idle-check')pulse();};
     addEventListener('message',message);
     requestIdleCallback(()=>{
      const end=performance.now()+80;while(performance.now()<end){}
      cancelIdleCallback(cancelled);pulse();
     });
     requestIdleCallback(deadline=>{
      if(!deadline.didTimeout||deadline.timeRemaining()!==0)return failed('runnable timeout');
      done=true;timed++;removeEventListener('message',message);
     },{timeout:20});
     cancelled=requestIdleCallback(()=>failed('cancelled runnable callback'),{timeout:20});
     requestIdleCallback(()=>{
      if(timed!==1)return failed('callback order or duplicate');
      document.title='WebCore idle check ready';
     });
    },50);
   });
  },100));
 });
})();
)JS"),
                                                  JSC::SourceTaintedOrigin::Untainted);
    for (unsigned attempt = 0;
         attempt < 10000 && frame.document()->title() != "WebCore idle check ready"_s; attempt++)
        tick();
    CHECK(frame.document()->title()=="WebCore idle check ready"_s);
    fprintf(
        stderr,
        "PASS real WebCore idle deadlines, microtasks, continuation, runnable timeout and cancellation\n");
    frame.script().executeScriptIgnoringException("document.title='WebCore clicked'"_s,
                                                  JSC::SourceTaintedOrigin::Untainted);
}

int main(int argc, char **argv) {
    while (argc > 1) {
        if (!strcmp(argv[argc - 1], "--exception-trace"))
            nativeExceptionTrace = true;
        else if (!strcmp(argv[argc - 1], "--client-navigation"))
            nativeClientNavigation = true;
        else
            break;
        argc--;
    }

    if ((argc == 5 || argc == 8 || argc == 12) && !strcmp(argv[1], "--view")) {
        if (argc >= 8)
            CHECK(ModernOS::setScreenSize(atoi(argv[5]),atoi(argv[6]),atoi(argv[7])));
        if (argc == 12)
            CHECK(ModernOS::setWindowGeometry(atoi(argv[8]),atoi(argv[9]),atoi(argv[10]),atoi(argv[11])));
        return nativeRun(argv[2], atoi(argv[3]), atoi(argv[4]));
    }

    if (argc == 18 && !strcmp(argv[1], "--subframe")) {
        CHECK(!nativeClientNavigation);
        if (argc >= 13)
            CHECK(ModernOS::setScreenSize(atoi(argv[10]),atoi(argv[11]),atoi(argv[12])));
        CHECK(ModernOS::setWindowGeometry(atoi(argv[13]),atoi(argv[14]),atoi(argv[15]),atoi(argv[16])));
        nativeEmbedding.enabled = true;
        nativeEmbedding.frame = strtoull(argv[5], nullptr, 10);
        nativeEmbedding.parent = strtoull(argv[6], nullptr, 10);
        nativeEmbedding.parentOrigin = URL{String::fromUTF8(argv[7])};
        nativeEmbedding.sandbox = strtoul(argv[8], nullptr, 10);
        nativeEmbedding.referrer = String::fromUTF8(argv[9]);
        nativeEmbedding.topOrigin = URL{String::fromUTF8(argv[17])};
        CHECK(nativeEmbedding.frame && nativeEmbedding.parent && nativeEmbedding.parentOrigin.protocolIsInHTTPFamily() &&
            nativeEmbedding.topOrigin.protocolIsInHTTPFamily());
        return nativeRun(argv[2], atoi(argv[3]), atoi(argv[4]));
    }

    alarm(120);
    CHECK(argc==3 && (!strcmp(argv[1],"--fixture") || !strcmp(argv[1],"--network-check")));
    bool network = !strcmp(argv[1], "--network-check");
    os_web_identity identity = {};
    CHECK(syscall(MODERNOS_WEB_IDENTITY,&identity)==0 && identity.role==OS_WEB_RENDERER);
    URL origin{String::fromUTF8(identity.origin)};
    URL url = network ? URL{String::fromUTF8(argv[2])}
                      : URL{makeString(String::fromUTF8(argv[2]), "/fixture.html"_s)};
    CHECK(url.isValid() && SecurityOrigin::create(url)->isSameOriginAs(SecurityOrigin::create(origin)));
    CHECK(write(0,&identity,sizeof(identity))==(ssize_t)sizeof(identity));
    errno = 0;
    CHECK(socket(AF_INET,SOCK_STREAM,0)==-1 && errno==EPERM);
    errno = 0;
    CHECK(open("/etc/ssl/cert.pem",O_RDONLY)==-1 && errno==EACCES);
    errno = 0;
    CHECK(open("/tmp/private-webcore",O_RDONLY)==-1 && errno==EACCES);

    ScriptController::initializeMainThread();
    bool observedIdle = false;
    RunLoopObserver observer(
        RunLoopObserver::WellKnownOrder::OpportunisticTask, [&] { observedIdle = true; },
        RunLoopObserver::Type::OneShot);
    observer.schedule();
    tick();
    CHECK(observedIdle);
    fprintf(stderr, "PASS native WebKit run-loop activity observers\n");

    std::unique_ptr<ModernOS::BrokerStrategies> brokerStrategies;
    std::unique_ptr<ModernOS::FixtureStrategies> fixtureStrategies;
    if (network) {
        brokerStrategies = std::make_unique<ModernOS::BrokerStrategies>(URL(origin));
        setPlatformStrategies(brokerStrategies.get());
    } else {
        fixtureStrategies = std::make_unique<ModernOS::FixtureStrategies>();
        setPlatformStrategies(fixtureStrategies.get());
    }

    CHECK(modernOSFonts());
    // Exercise the genuine empty-typeface/zero-size cache key even when the
    // packaged generic-font aliases provide an ordinary fallback face.
    FontPlatformData zero(SkTypeface::MakeEmpty(), 0, false, false, FontOrientation::Horizontal,
                          FontWidthVariant::RegularWidth, TextRenderingMode::Auto, {});
    auto zeroFont = FontCache::forCurrentThread().fontForPlatformData(zero);
    auto sameZero = FontCache::forCurrentThread().fontForPlatformData(zero);
    CHECK(zeroFont.ptr()==sameZero.ptr() && zeroFont->platformData().size()==0);
    fprintf(stderr,
            "PASS zero-sized empty-typeface font cache and packaged generic-font aliases\n");

    auto config =
        pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    config.editorClient = makeUniqueRef<ModernOS::EditorModernOS>();
    config.chromeClient = makeUniqueRef<ModernOS::ChromeModernOS>(viewState);
    config.mainFrameCreationParameters = PageConfiguration::LocalMainFrameCreationParameters{
        [url, network](LocalFrame &frame,
                       FrameLoader &loader) -> UniqueRef<LocalFrameLoaderClient> {
            return makeUniqueRefWithoutRefCountedCheck<FixtureFrameClient>(frame, loader, url,
                                                                           network);
        },
        {},
        ReferrerPolicy::EmptyString};

    Ref page = Page::create(WTF::move(config));
    activePage = page.ptr();
    page->settings().setScriptEnabled(true);
    // Explicitly test the genuine experimental API in controlled components;
    // the native browser retains the upstream default preference.
    page->settings().setRequestIdleCallbackEnabled(true);
    page->settings().setImagesEnabled(true);
    page->settings().setLoadsImagesAutomatically(true);
    page->settings().setAcceleratedCompositingEnabled(false);
    page->settings().setLocalStorageEnabled(false);
    page->settings().setSessionStorageEnabled(false);
    page->settings().setIndexedDBAPIEnabled(false);
    page->settings().setServiceWorkersEnabled(false);
    page->settings().setStandardFontFamily(AtomString("DejaVu Sans"_s));
    page->settings().setSansSerifFontFamily(AtomString("DejaVu Sans"_s));

    RefPtr frame = page->localMainFrame();
    CHECK(frame);
    frame->init();
    frame->createView({fixtureWidth, fixtureHeight}, Color::white, {});
    page->setIsInWindow(true);
    page->setActivityState({ActivityState::IsVisible, ActivityState::IsInWindow,
                            ActivityState::IsFocused, ActivityState::WindowIsActive});

    static constexpr char html[] =
        R"HTML(<!doctype html><meta charset="utf-8"><title>Loading fixture</title>
<style>body{margin:0;background:#fafafa;font:16px "DejaVu Sans"}#grid{position:absolute;left:20px;top:20px;display:grid;grid-template-columns:40px 40px;gap:10px}#grid div{height:40px}#marker{position:absolute;left:140px;top:100px;width:40px;height:40px;background:#ffcc00}button{position:absolute;left:20px;top:100px;width:80px;height:40px;border:0;background:#778899}canvas{position:absolute;left:220px;top:20px}p{position:absolute;left:20px;top:160px}</style>
<div id="grid"><div style="background:#e02030"></div><div style="background:#2040e0"></div></div>
<button id="change">Change</button><div id="marker"></div><canvas id="canvas" width="40" height="40"></canvas><p>WebCore office السلام</p>
<script>const [a,b]=[2,3];const c=document.getElementById('canvas').getContext('2d');c.fillStyle='#d030c0';c.fillRect(0,0,40,40);document.getElementById('change').addEventListener('click',()=>{document.getElementById('marker').style.background='#00cc44';document.title='WebCore clicked';});Promise.resolve(a+b).then(v=>{if(v===5)document.title='WebCore rendered';});</script>)HTML";
    auto buffer =
        SharedBuffer::create(std::span{reinterpret_cast<const uint8_t *>(html), sizeof(html) - 1});
    ResourceResponse response(URL(url), "text/html"_s, sizeof(html) - 1, "UTF-8"_s);
    SubstituteData data(WTF::move(buffer), {}, WTF::move(response),
                        SessionHistoryVisibility::Visible);
    if (network)
        frame->loader().load(FrameLoadRequest(*frame, ResourceRequest(URL(url))));
    else
        frame->loader().load(FrameLoadRequest(*frame, ResourceRequest(URL(url)), WTF::move(data)));

    unsigned attempts = 0;
    while (frame->document()->title() != "WebCore rendered"_s && !loadFailed && ++attempts < 10000)
        tick();
    if (loadFailed || frame->document()->title() != "WebCore rendered"_s)
        fprintf(stderr, "WebCore document title at failure: %s\n",
                frame->document()->title().utf8().data());
    CHECK(!loadFailed && frame->document()->title()=="WebCore rendered"_s && frame->view());
    CHECK(frame->document()->securityOrigin().isSameOriginAs(SecurityOrigin::create(url)));

    emit(*frame);
    click(*frame, {50, 120});
    for (unsigned i = 0; i < 20; i++)
        tick();
    CHECK(frame->document()->title()=="WebCore clicked"_s);
    emit(*frame);

    checkGarbageCollection(*frame);
    checkIdleCallbacks(*frame);

    if (network) {
        click(*frame, {50, 155});
        for (char c : std::string("webki"))
            key(*frame, String::fromUTF8(std::span{&c, 1}), String::fromUTF8(std::span{&c, 1}),
                toupper(c));
        auto input = [&]() -> HTMLInputElement * {
            return dynamicDowncast<HTMLInputElement>(
                frame->document()->getElementById(AtomString("field"_s)));
        };

        CHECK(input() && String(input()->value())=="webki"_s);
        key(*frame, "\b"_s, "Backspace"_s, 8);
        CHECK(String(input()->value())=="webk"_s);
        key(*frame, "i"_s, "i"_s, 'I');
        key(*frame, "t"_s, "t"_s, 'T');
        for (unsigned i = 0; i < 20; i++)
            tick();
        CHECK(String(input()->value())=="webkit"_s && frame->document()->title()=="WebCore typed: webkit"_s);
        emit(*frame);

        wheel(*frame, -480);
        for (unsigned i = 0; i < 300 && frame->view()->scrollY() < 400; i++)
            tick();
        if (frame->view()->scrollY() < 400 || frame->view()->scrollY() > 520)
            fprintf(stderr, "Wheel position=%d contentsHeight=%d visibleHeight=%d\n",
                    frame->view()->scrollY(), frame->view()->contentsHeight(),
                    frame->view()->visibleHeight());
        CHECK(frame->view()->scrollY()>=400 && frame->view()->scrollY()<=520);
        emit(*frame);

        wheel(*frame, 480);
        for (unsigned i = 0; i < 300 && frame->view()->scrollY(); i++)
            tick();
        CHECK(frame->view()->scrollY()==0);

        click(*frame, {50, 155});
        key(*frame, "\r"_s, "Enter"_s, 13);
        attempts = 0;
        while (frame->document()->title() != "WebCore form submitted"_s && !loadFailed &&
               ++attempts < 10000)
            tick();
        CHECK(!loadFailed && frame->document()->title()=="WebCore form submitted"_s);
        CHECK(frame->document()->url().path()=="/webcore-form"_s && frame->document()->securityOrigin().isSameOriginAs(SecurityOrigin::create(origin)));
        emit(*frame);
        fprintf(
            stderr,
            "PASS actual brokered WebCore HTML/CSS/script/fetch/PNG, editing/backspace, wheel pixels and form POST/navigation\n");
    }

    fprintf(
        stderr,
        "PASS genuine WebCore fixture DOM/Grid/canvas/promise and mouse click; native/public browser remains unfinished\n");
    return 0;
}
