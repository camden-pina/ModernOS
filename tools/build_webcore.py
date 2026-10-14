#!/usr/bin/env python3
"""Build genuine WebCore/JSC with the ModernOS software platform adaptation.

Generated copies are isolated from the already tested JSC source/build. Owned
port files and checked patches are reapplied without re-extracting old caches.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import tarfile

from build_threaded_libgcc import build_posix_libgcc

from build_webcore_deps import ROOT, BUILD, PREFIX, WEB_DEPS, FLAGS, CROSS, environment, run, verify

SOURCE = BUILD / 'webcore-source/wpewebkit-2.54.0'
PORT = ROOT / 'userland/modern-browser/port'
ARCHIVE_HASH = 'efa9bcc3cb891c2d88f50eec710d9ccee71cbdf1040420361eb98c17355eb452'


def write_if_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def patch(relative, transform):
    path = SOURCE / relative
    original = BUILD / 'webcore-original/upstream' / relative
    if not original.exists():
        original.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, original)
    text = transform(original.read_text())
    if path.read_text() != text:
        path.write_text(text)


def replace(text, old, new):
    if text.count(old) != 1:
        raise SystemExit('Pinned WebCore patch drift: ' + old[:100])
    return text.replace(old, new)


def function_body(text, signature, body):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    closing = opening + 1
    while depth:
        depth += (text[closing] == '{') - (text[closing] == '}')
        closing += 1
    return text[:opening] + '{\n' + body + '\n}' + text[closing:]


def notices():
    licenses = ROOT / 'userland/vendor/licenses'
    sections = [
        'WebCore and bundled CPU Skia/xdgmime notices, WPE WebKit 2.54.0.\n'
        'Corresponding source: userland/vendor/wpewebkit-2.54.0.tar.xz\n'
        f'SHA-256: {ARCHIVE_HASH}\n'
        'ModernOS adaptations: tools/build_webcore.py and userland/modern-browser/port/\n',
        (SOURCE / 'Source/JavaScriptCore/COPYING.LIB').read_text(),
    ]
    seen = set()
    for component in ('WebCore', 'ThirdParty/skia', 'ThirdParty/xdgmime'):
        for path in sorted((SOURCE / 'Source' / component).rglob('*')):
            if not path.is_file():
                continue
            block = None

            if path.name.startswith(('LICENSE', 'COPYING')):
                block = path.read_text(errors='replace')
            elif path.suffix in ('.c', '.cpp', '.h', '.hpp', '.mm', '.m', '.js', '.rb'):
                with path.open(errors='replace') as source:
                    match = re.match(r'\s*/\*.*?\*/', source.read(32768), re.DOTALL)
                if match and 'copyright' in match[0].lower():
                    block = match[0].strip()

            if block and block not in seen:
                seen.add(block)
                sections.append(f'\n--- {path.relative_to(SOURCE)} ---\n{block}\n')
    write_if_changed(licenses / 'webcore-NOTICES.txt', ''.join(sections))


def prepare():
    archive = ROOT / 'userland/vendor/wpewebkit-2.54.0.tar.xz'
    verify(archive, ARCHIVE_HASH)
    if not SOURCE.exists():
        SOURCE.parent.mkdir(exist_ok=True)
        with tarfile.open(archive) as contents:
            contents.extractall(SOURCE.parent, filter='data')

    for src, dst in (
        ('OptionsModernOS.cmake', 'Source/cmake/OptionsModernOS.cmake'),
        ('PlatformModernOS.cmake', 'Source/WebCore/PlatformModernOS.cmake'),
        ('SourcesModernOS.txt', 'Source/WebCore/SourcesModernOS.txt'),
    ):
        path = SOURCE / dst
        data = (PORT / src).read_bytes()
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)

    patch('Source/cmake/WebKitCommon.cmake', lambda t: replace(t,
          '        WPE\n', '        WPE\n        ModernOS\n'))

    # Use the already tested POSIX low-level implementations and real GLib loop.
    write_if_changed(SOURCE / 'Source/WTF/wtf/PlatformModernOS.cmake', '''include(PlatformWPE.cmake)
list(REMOVE_ITEM WTF_SOURCES linux/CurrentProcessMemoryStatus.cpp)
list(APPEND WTF_SOURCES "${MODERNOS_PORT_DIR}/MemoryStatusModernOS.cpp")
''')
    write_if_changed(SOURCE / 'Source/JavaScriptCore/PlatformModernOS.cmake', '')

    # WebCore's worker startup also obtains a database connection when the
    # IndexedDB API is disabled. EmptyDatabaseProvider deliberately has no
    # connection identifier. A disabled service must yield no proxy, allowing
    # real workers to run without fabricating a database implementation.
    patch('Source/WebCore/workers/WorkerMessagingProxy.cpp', lambda t: replace(t,
                                                                               '    return &page->idbConnection().proxy();',
                                                                               '''#if MODERNOS_RASTER_ONLY
    if (!document->settings().indexedDBAPIEnabled())
        return nullptr;
#endif
    return &page->idbConnection().proxy();'''))

    # ModernOS eagerly backs allocations and has no Linux MADV_DONTNEED.
    # These page-aligned ranges belong entirely to bmalloc's caller. Clear
    # the actual bytes while preserving its paired allocation/free API.
    patch('Source/bmalloc/bmalloc/bmalloc.cpp', lambda t: replace(replace(t,
                                                                          '#include "bmalloc.h"', '#include "bmalloc.h"\n#include <cstring>'),
                                                                  '    if (result)\n        vmZeroAndPurge(result, size);',
                                                                  '''    if (result) {
#if MODERNOS_RASTER_ONLY
        std::memset(result, 0, size);
#else
        vmZeroAndPurge(result, size);
#endif
    }'''))
    write_if_changed(SOURCE / 'Source/WebCore/PAL/pal/PlatformModernOS.cmake',
                     'include(PlatformWPE.cmake)\n')
    patch('Source/WebCore/CMakeLists.txt', lambda t: replace(t,
                                                             'else ()\n    list(APPEND WebCore_LIBRARIES OpenGL::GLES)',
                                                             'elseif (NOT MODERNOS_RASTER_ONLY)\n    list(APPEND WebCore_LIBRARIES OpenGL::GLES)') +
          '\ninclude("${MODERNOS_PORT_DIR}/Fixture.cmake")\n')

    def extensible_frame_client(t):
        for name in ('hasWebView', 'canHandleRequest', 'canShowMIMEType', 'committedLoad',
                     'transitionToCommittedForNewPage', 'dispatchDecidePolicyForResponse',
                     'dispatchDecidePolicyForNavigationAction', 'dispatchDidFailProvisionalLoad', 'dispatchDidFailLoad',
                     'dispatchDidStartProvisionalLoad', 'dispatchDidReceiveTitle', 'dispatchDidCommitLoad',
                     'dispatchDidFinishLoad', 'dispatchDidChangeLocationWithinPage',
                     'dispatchDidPushStateWithinPage', 'dispatchDidReplaceStateWithinPage',
                     'canCachePage', 'createFrame', 'didRestoreFromBackForwardCache',
                     'createHistoryItemTree', 'dispatchDidReachLayoutMilestone',
                     'shouldGoToHistoryItem', 'dispatchGoToBackForwardItemAtIndex',
                     'dispatchEnqueueHistoryTraversalDelta', 'dispatchLoadEventToOwnerElementInAnotherProcess'):
            pattern = rf'(\b{name}\([^\n]*\)(?: const)?) final;'
            t, count = re.subn(pattern, r'\1 override;', t)
            if count != 1:
                raise SystemExit('Pinned frame client extension drift: ' + name)
        return t

    patch('Source/WebCore/loader/EmptyFrameLoaderClient.h', extensible_frame_client)

    # Console previews must not run page code: the original object-to-string
    # conversion invokes toString/Symbol.toPrimitive and Error getters. Fix
    # the shared JSC formatter so frame, worker and inspector messages agree.
    # JSC's sanitized Error preview explicitly avoids getters and proxies.
    patch('Source/JavaScriptCore/inspector/ScriptArguments.cpp', lambda t: replace(replace(t,
                                                                                           '#include "ProxyObject.h"', '#include "ErrorInstance.h"\n#include "ProxyObject.h"'),
                                                                                   '    auto result = argument.toWTFString(globalObject);',
                                                                                   '''    if (auto* error = dynamicDowncast<JSC::ErrorInstance>(argument)) {
        auto result = error->sanitizedToString(globalObject);
        scope.clearException();
        return result;
    }
    if (argument.isObject())
        return "[object]"_s;
    auto result = argument.toWTFString(globalObject);'''))

    # Retain primitive arguments in the native serial log, including values
    # after a format string. The shared formatter supplies safe object previews.
    patch('Source/WebCore/page/FrameConsoleClient.cpp', lambda t: replace(replace(t,
                                                                                  '#include <wtf/text/WTFString.h>', '#include <wtf/text/StringBuilder.h>\n#include <wtf/text/WTFString.h>'),
                                                                          'page->chrome().client().addMessageToConsole(MessageSource::ConsoleAPI, level, messageText, lineNumber, columnNumber, url);',
                                                                          '''StringBuilder consoleText;
        for (const auto& argument : arguments->getArgumentsAsStrings()) {
            if (consoleText.length())
                consoleText.append(' ');
            consoleText.append(argument.left(4096 - consoleText.length()));
            if (consoleText.length() >= 4096)
                break;
        }
        page->chrome().client().addMessageToConsole(MessageSource::ConsoleAPI, level, consoleText.toString(), lineNumber, columnNumber, url);'''))
    patch('Source/WebCore/platform/graphics/MediaPlayer.h', lambda t:
          replace(replace(t, 'enum class MediaPlayerType {\n', 'enum class MediaPlayerType {\n    ModernOS,\n'),
                  'enum class MediaPlatformType {\n', 'enum class MediaPlatformType {\n    ModernOS,\n'))
    patch('Source/WebCore/platform/MediaSample.h', lambda t:
          replace(replace(t, 'using VariantType = Variant<const MockSampleBox*',
                          'using VariantType = Variant<const MockSampleBox*\n        , const void*'),
                  'enum class Type : uint8_t {\n', 'enum class Type : uint8_t {\n        ModernOS,\n'))
    patch('Source/WebCore/platform/graphics/MediaPlayerEnums.h', lambda t:
          replace(t, 'enum class MediaPlayerMediaEngineIdentifier : uint8_t {\n',
                  'enum class MediaPlayerMediaEngineIdentifier : uint8_t {\n    ModernOS,\n'))

    # Structured clones travel through the trusted frame broker. Transferable
    # capabilities need their own lifetime protocol; reject those before
    # serialization can detach a sender's buffer or message port.
    patch('Source/WebCore/page/RemoteDOMWindow.cpp', lambda t: replace(replace(replace(t,
          '#include "MessagePort.h"', '#include "MessagePort.h"\n#include "NonSerializedDataToken.h"'),
                                                                               '    Vector<Ref<MessagePort>> ports;\n',
                                                                               '''    if (!options.transfer.isEmpty())
        return Exception { ExceptionCode::DataCloneError, "Transferables between isolated frames are not supported."_s };
    Vector<Ref<MessagePort>> ports;
'''),
                                                                       '    auto disentangledPorts = MessagePort::disentanglePorts(WTF::move(ports));',
                                                                       '''    if (messageData.returnValue()->hasBlobURLs() || messageData.returnValue()->nonSerializedDataToken())
        return Exception { ExceptionCode::DataCloneError, "This structured clone requires unsupported frame capabilities."_s };
    auto disentangledPorts = MessagePort::disentanglePorts(WTF::move(ports));'''))

    # View transitions request a compositor even when ordinary accelerated
    # compositing is disabled. A CPU port must skip the visual transition,
    # while still running the real update callback and settling its promises.
    patch('Source/WebCore/dom/Document.cpp', lambda t: replace(replace(replace(t,
          '    if (hidden()) {\n        viewTransition->skipViewTransition',
                                                                               '''
    if (!settings().acceleratedCompositingEnabled()) {
        viewTransition->skipViewTransition(Exception { ExceptionCode::NotSupportedError, "This page uses CPU rendering without a view-transition compositor."_s });
        return viewTransition;
    }
    if (hidden()) {
        viewTransition->skipViewTransition'''),
                                                                       'Variant<Document::SkipTransition, Vector<AtomString>> Document::resolveViewTransitionRule()\n{',
                                                                       '''Variant<Document::SkipTransition, Vector<AtomString>> Document::resolveViewTransitionRule()
{
    if (!settings().acceleratedCompositingEnabled())
        return SkipTransition { };'''),
                                                               'IDBClient::IDBConnectionProxy* Document::idbConnectionProxy()\n{',
                                                               '''IDBClient::IDBConnectionProxy* Document::idbConnectionProxy()
{
#if MODERNOS_RASTER_ONLY
    if (!settings().indexedDBAPIEnabled())
        return nullptr;
#endif'''))
    patch('Source/JavaScriptCore/heap/ConservativeRoots.cpp', lambda t: t)

    # Native pointer routing enters the embedding frame's EventHandler before
    # forwarding to an isolated child. Activate only the actual local target;
    # the child broker supplies the ancestors' activation afterward. Otherwise
    # a foreign click also activates the parent's same-origin descendants.

    def pointer_activation_target(t):
        start = t.index(
            'HandleUserInputEventResult EventHandler::handleMousePressEvent(const PlatformMouseEvent&')
        end = t.index('HandleUserInputEventResult EventHandler::handleMouseReleaseEvent(', start)
        body = t[start:end]
        gesture = ('    UserGestureIndicator gestureIndicator(IsProcessingUserGesture::Yes, '
                   'protect(frame->document()).get(), userGestureTypeForPlatformEvent(platformMouseEvent), '
                   'UserGestureIndicator::ProcessInteractionStyle::Immediate, platformMouseEvent.authorizationToken());\n')
        body = replace(body, gesture, '')
        marker = '#if ENABLE(PAN_SCROLLING)\n    // We store whether pan scrolling is in progress'
        body = replace(body, marker, gesture + '\n' + marker)
        return t[:start] + body + t[end:]

    patch('Source/WebCore/page/EventHandler.cpp', pointer_activation_target)

    # Continue a real idle period after an earlier callback uses its deadline.
    # The pinned upstream loop otherwise leaves runnable callbacks with no
    # timer, task or animation to wake them. Keep the normal work/deadline
    # checks in WindowEventLoop; do not manufacture an idle deadline.
    patch('Source/WebCore/dom/WindowEventLoop.cpp', lambda t: replace(replace(t,
          '#include "IdleCallbackController.h"',
                                                                              '#include "IdleCallbackController.h"\n#include "FrameDestructionObserverInlines.h"'),
                                                                      '            idleCallbackController->startIdlePeriod();\n',
                                                                      '''            idleCallbackController->startIdlePeriod();
        if (document->frame() && document->hasPendingIdleCallback())
            scheduleIdlePeriod();
'''))

    # A request moves to the runnable queue at the beginning of an idle
    # period. Its real timeout must still be able to remove and invoke it
    # exactly once from either queue (requestidlecallback section 5.3).

    def idle_callback_timeouts(t):
        t = replace(t, '''    auto it = m_idleRequestCallbacks.findIf([identifier](auto& request) {
        return request.identifier == identifier;
    });

    if (it == m_idleRequestCallbacks.end())
        return;

    auto idleDeadline = IdleDeadline::create(IdleDeadline::DidTimeout::Yes);
    auto callback = WTF::move(it->callback);
    m_idleRequestCallbacks.remove(it);
    callback->invoke(idleDeadline.get());''', '''    auto takeCallback = [identifier](auto& callbacks) -> RefPtr<IdleRequestCallback> {
        auto it = callbacks.findIf([identifier](auto& request) {
            return request.identifier == identifier;
        });
        if (it == callbacks.end())
            return nullptr;
        auto callback = WTF::move(it->callback);
        callbacks.remove(it);
        return callback;
    };
    auto callback = takeCallback(m_idleRequestCallbacks);
    if (!callback)
        callback = takeCallback(m_runnableIdleCallbacks);
    if (!callback)
        return;

    auto idleDeadline = IdleDeadline::create(IdleDeadline::DidTimeout::Yes);
    callback->invoke(idleDeadline.get());''')

        # Deliver one callback per actual idle task, allowing microtask
        # checkpoints and higher-priority work between callbacks (section 5.2).
        t = replace(t, '    while (invokeIdleCallbacks()) { }\n}',
                    '    queueTaskToInvokeIdleCallbacks();\n}')
        t = replace(t, '''void IdleCallbackController::queueTaskToInvokeIdleCallbacks()
{
    Ref document = *m_document;''', '''void IdleCallbackController::queueTaskToInvokeIdleCallbacks()
{
    if (m_idleCallbackTaskQueued)
        return;
    m_idleCallbackTaskQueued = true;
    Ref document = *m_document;''')
        t = replace(t, '''        RELEASE_ASSERT(document->idleCallbackController() == checkedThis.get());
        while (checkedThis->invokeIdleCallbacks()) { }''', '''        RELEASE_ASSERT(document->idleCallbackController() == checkedThis.get());
        checkedThis->m_idleCallbackTaskQueued = false;
        checkedThis->invokeIdleCallbacks();''')
        t = replace(t, '''    if (now >= deadline || m_runnableIdleCallbacks.isEmpty())
        return false;''', '''    if (now >= deadline || m_runnableIdleCallbacks.isEmpty()) {
        if (!m_runnableIdleCallbacks.isEmpty())
            windowEventLoop->scheduleIdlePeriod();
        return false;
    }''')
        t = replace(t,
                    'IdleDeadline::create(request.timeout && *request.timeout < now ? IdleDeadline::DidTimeout::Yes : IdleDeadline::DidTimeout::No)',
                    'IdleDeadline::create(IdleDeadline::DidTimeout::No)')
        return replace(t, '    return !m_runnableIdleCallbacks.isEmpty();',
                       '''    if (!m_runnableIdleCallbacks.isEmpty() && document->frame())
        queueTaskToInvokeIdleCallbacks();
    return !m_runnableIdleCallbacks.isEmpty();''')

    patch('Source/WebCore/dom/IdleCallbackController.cpp', idle_callback_timeouts)
    patch('Source/WebCore/dom/IdleCallbackController.h', lambda t: replace(t,
          '    unsigned m_idleCallbackIdentifier { 0 };',
                                                                           '    unsigned m_idleCallbackIdentifier { 0 };\n    bool m_idleCallbackTaskQueued { false };'))

    # Zero-sized text with an unavailable font is a valid page request. Skia's
    # empty typeface at size zero otherwise compares equal to WebCore's old
    # hash-table empty key, causing the genuine cache validation to abort.
    # Negative font sizes are not valid rendered fonts, so use -1 as sentinel.
    patch('Source/WebCore/platform/graphics/FontCache.cpp', lambda t:
          replace(t, 'static NeverDestroyed<FontPlatformData> key(0.f, false, false);',
                  'static NeverDestroyed<FontPlatformData> key(-1.f, false, false);'))

    # The software port must consult the real page preference for wheel
    # animation too. Without this override the controller's default is true,
    # even when Settings::scrollAnimatorEnabled() is false.
    patch('Source/WebCore/platform/ScrollAnimator.h', lambda t: replace(t,
          '#if USE(COORDINATED_GRAPHICS)\n    bool scrollAnimationEnabled() const final;',
                                                                        '#if USE(COORDINATED_GRAPHICS) || MODERNOS_RASTER_ONLY\n    bool scrollAnimationEnabled() const final;'))
    patch('Source/WebCore/platform/ScrollAnimator.cpp', lambda t: replace(t,
          '#if USE(COORDINATED_GRAPHICS)\nbool ScrollAnimator::scrollAnimationEnabled() const',
                                                                          '#if USE(COORDINATED_GRAPHICS) || MODERNOS_RASTER_ONLY\nbool ScrollAnimator::scrollAnimationEnabled() const'))

    # WPE's view-sized screen fallback is wrong for a desktop window and for
    # independently rendered iframes. Use the actual desktop display metadata.
    patch('Source/WebCore/platform/wpe/PlatformScreenWPE.cpp', lambda t: replace(replace(replace(t,
          '#include "config.h"', f'#include "config.h"\n#include "{PORT}/ScreenModernOS.h"'),
        'FloatRect screenRect(Widget* widget)\n{',
        'FloatRect screenRect(Widget* widget)\n{\n    if (ModernOS::screenWidth && ModernOS::screenHeight)\n'
        '        return { 0, 0, static_cast<float>(ModernOS::screenWidth), static_cast<float>(ModernOS::screenHeight) };'),
        'FloatRect screenAvailableRect(Widget* widget)\n{',
        'FloatRect screenAvailableRect(Widget* widget)\n{\n    if (ModernOS::screenWidth && ModernOS::screenAvailableHeight)\n'
        '        return { 0, 0, static_cast<float>(ModernOS::screenWidth), static_cast<float>(ModernOS::screenAvailableHeight) };'))

    # Canvas font setters accept a valid zero size, just as CSS text does.
    # Retaining the inherited size here produces visible ink and wrong metrics.
    patch('Source/WebCore/style/StyleResolveForFont.cpp', lambda t: replace(t,
          'if (resolvedSize.size > 0) {', 'if (resolvedSize.size >= 0) {'))
    patch('Source/WebCore/platform/PlatformKeyboardEvent.cpp', lambda t: replace(t,
                                                                                 '#if PLATFORM(GTK) || USE(LIBWPE) || ENABLE(WPE_PLATFORM)',
                                                                                 '#if PLATFORM(GTK) || USE(LIBWPE) || ENABLE(WPE_PLATFORM) || MODERNOS_RASTER_ONLY'))
    keyboard = (SOURCE / 'Source/WebCore/platform/libwpe/PlatformKeyboardEventLibWPE.cpp').read_text()
    license_text = keyboard[:keyboard.index('#include "config.h"')]
    write_if_changed(SOURCE / 'Source/WebCore/platform/wpe/PlatformKeyboardEventModernOS.cpp',
                     license_text + '''#include "config.h"
#include "PlatformKeyboardEvent.h"
namespace WebCore {
// Preserve upstream's KeyDown-to-RawKeyDown/Char split. Native callers supply
// actual key text/codes/modifiers through the public platform event constructor.
void PlatformKeyboardEvent::disambiguateKeyDownEvent(Type type, bool backwardsCompatibility)
{
    ASSERT(m_type == PlatformEvent::Type::KeyDown);
    m_type = type;
    if (backwardsCompatibility)
        return;
#if PLATFORM(GTK) || USE(LIBWPE) || ENABLE(WPE_PLATFORM)
    if (m_handledByInputMethod)
        return;
#endif
    if (type == PlatformEvent::Type::RawKeyDown) {
        m_text = String();
        m_unmodifiedText = String();
    } else
        m_keyIdentifier = String();
}
}
''')
    editing = (SOURCE / 'Source/WebKit/WebProcess/WebCoreSupport/wpe/WebEditorClientWPE.cpp').read_text()
    editing = editing.replace('#include "WebEditorClient.h"\n',
                              '').replace('#include "WebPage.h"\n', '')
    editing = replace(editing, 'namespace WebKit {', 'namespace ModernOS {')
    editing = replace(editing, 'void WebEditorClient::handleKeyboardEvent(WebCore::KeyboardEvent& event)',
                      'void handleModernOSKeyboardEvent(WebCore::KeyboardEvent& event)')
    write_if_changed(SOURCE / 'Source/WebCore/platform/wpe/KeyboardEditingModernOS.cpp', editing)

    # Reuse upstream's portable native accessibility object model/Chrome
    # notifications. It owns real wrapper lifetimes without AT-SPI/D-Bus.
    # The native desktop accessibility consumer remains unimplemented.
    for relative in ('Source/WebCore/accessibility/AXCoreObject.h',
                     'Source/WebCore/page/ChromeClient.h',
                     'Source/WebCore/loader/EmptyClients.h'):
        def portable_client(t, relative=relative):
            t = t.replace('PLATFORM(PLAYSTATION) || PLATFORM(HAIKU)',
                          'PLATFORM(PLAYSTATION) || PLATFORM(HAIKU) || MODERNOS_RASTER_ONLY')
            if relative.endswith('EmptyClients.h'):
                for name in ('isEmptyChromeClient', 'windowRect', 'pageRect', 'addMessageToConsole', 'invalidateRootView',
                             'invalidateContentsForSlowScroll', 'scroll', 'triggerRenderingUpdate',
                             'contentsSizeChanged', 'focusedFrameChanged', 'keyboardUIMode', 'hoverSupportedByPrimaryPointingDevice',
                             'hoverSupportedByAnyAvailablePointingDevice', 'pointerCharacteristicsOfPrimaryPointingDevice',
                             'pointerCharacteristicsOfAllAvailablePointingDevices'):
                    pattern = rf'(\b{name}\([^\n]*\)(?: const)?) final'
                    t, count = re.subn(pattern, r'\1 override', t)
                    if count != 1:
                        raise SystemExit('Pinned Chrome client extension drift: ' + name)
            return t
        patch(relative, portable_client)

    # Match the guard used by the generated media bindings in configurations
    # that explicitly disable video. Native builds include the real media DOM.
    patch('Source/WebCore/bindings/js/JSHTMLMediaElementCustom.cpp', lambda t:
          replace(t, '#include "config.h"', '#include "config.h"\n#if ENABLE(VIDEO)') + '\n#endif // ENABLE(VIDEO)\n')

    def raster_skia(t):
        start = t.index('if (USE_LIBEPOXY)')
        end = t.index('\nif (USE_SKIA_ENCODERS)', start)
        t = t[:start] + t[end:]
        t = replace(t, 'target_compile_definitions(Skia PRIVATE\n    SK_GL\n    SK_GANESH\n)',
                    'target_compile_definitions(Skia PUBLIC SK_ENABLE_OPTIMIZE_SIZE SK_DISABLE_AVX512_OPTS)')
        t = replace(t, 'if (WTF_CPU_X86 OR WTF_CPU_X86_64)',
                    'if (FALSE) # ModernOS: FXSAVE/SSE2 context, no AVX targets')

        # Drop Ganesh and AVX implementation objects. CPU raster/shaders/filters
        # remain real upstream Skia; optimize-size disables runtime SIMD swaps.
        t += '''\nget_target_property(modernos_skia_sources Skia SOURCES)
list(FILTER modernos_skia_sources EXCLUDE REGEX "src/gpu/ganesh/|src/opts/SkOpts_(ml3|ml4|lasx)\\\\.cpp|src/core/SkMemset_opts_avx\\\\.cpp")
set_property(TARGET Skia PROPERTY SOURCES ${modernos_skia_sources})
'''
        return t

    patch('Source/ThirdParty/skia/CMakeLists.txt', raster_skia)

    # GLib and WebCore both bundle real xdgmime. Two private symbols upstream
    # omits from XDG_PREFIX need distinct names in this static executable.
    patch('Source/ThirdParty/xdgmime/CMakeLists.txt', lambda t: t + '''
target_compile_definitions(xdgmime PRIVATE
    _caches=_wk_xdg_caches
    _xdg_binary_or_text_fallback=_wk_xdg_binary_or_text_fallback)
''')

    # Compile only actual CPU backends. GPU-only sources do not enter this port.
    excluded = {'GraphicsContextGLSkia.cpp', 'ImageBufferSkiaAcceleratedBackend.cpp',
                'PlatformDisplaySkia.cpp', 'SkiaUtilities.cpp', 'SkiaGPUAtlas.cpp',
                'SkiaRecordingResult.cpp', 'SkiaReplayAtlas.cpp', 'SkiaReplayCanvas.cpp',
                'SkiaPaintingEngine.cpp', 'SkiaSerializedImageBuffer.cpp',
                'SkiaImageAtlasLayout.cpp', 'SkiaImageAtlasLayoutBuilder.cpp'}
    patch('Source/WebCore/platform/SourcesSkia.txt', lambda t: '\n'.join(
        line for line in t.splitlines() if Path(line.split(' ')[0]).name not in excluded) +
        '\nplatform/graphics/skia/SkiaRasterUtilities.cpp\n')

    graphics = 'Source/WebCore/platform/graphics/skia/'

    def raster_context(t):
        for header in ('GLContext.h', 'PlatformDisplay.h', 'ProcessCapabilities.h', 'SkiaPaintingEngine.h'):
            t = t.replace(f'#include "{header}"\n', '')
        t = t.replace('#include "SkiaImageAtlasLayoutBuilder.h"\n', '')
        t = t.replace('#include <skia/gpu/ganesh/SkSurfaceGanesh.h>\n', '')
        t = function_body(t, 'bool GraphicsContextSkia::makeGLContextCurrentIfNeeded() const',
                          '    // This port constructs only CPU-backed contexts.\n'
                          '    return m_renderingMode == RenderingMode::Unaccelerated;')
        t = function_body(t, 'sk_sp<SkImage> GraphicsContextSkia::imageForCurrentThread(',
                          '    RELEASE_ASSERT(!image->isTextureBacked());\n    return image;')
        t = function_body(t, 'static sk_sp<SkSurface> createAcceleratedSurface(',
                          '    UNUSED_PARAM(size);\n    return nullptr; // GPU surfaces are unavailable.')
        t = function_body(t, 'void GraphicsContextSkia::trackAcceleratedRenderingFenceIfNeeded(const sk_sp<SkImage>&',
                          '    RELEASE_ASSERT(!grContext);\n    RELEASE_ASSERT(!image || !image->isTextureBacked());')
        t = function_body(t, 'void GraphicsContextSkia::beginRecording(',
                          '    RELEASE_ASSERT(recordingMode == RecordingMode::Canvas);\n'
                          '    RELEASE_ASSERT(!threadSafeGrContext);\n'
                          '    ASSERT(m_contextMode == ContextMode::PaintingMode);\n'
                          '    m_contextMode = ContextMode::CanvasRecordingMode;\n'
                          '    if (!m_enableStateReplayTracking) {\n'
                          '        m_enableStateReplayTracking = true;\n        pushSkiaState();\n    }')
        t = function_body(t, 'SkiaRecordingData GraphicsContextSkia::endRecording()',
                          '    RELEASE_ASSERT(m_contextMode == ContextMode::CanvasRecordingMode);\n'
                          '    m_contextMode = ContextMode::PaintingMode;\n    return { };')
        start = t.index('    // Collect raster images for atlas batching during recording.')
        end = t.index('\n    auto imageSize = nativeImage.size();', start)
        t = t[:start] + t[end:]

        # Texture-backed tiles cannot occur in this CPU-only port. Keep the
        # genuine SkPicture raster tiling branch, including spacing/clipping.
        start = t.index(
            '        if (image->isTextureBacked()) {', t.index('void GraphicsContextSkia::drawPattern'))
        end = t.index('        } else {', start) + len('        } else {')
        t = t[:start] + '        RELEASE_ASSERT(!image->isTextureBacked());\n        {' + t[end:]
        return t

    patch(graphics + 'GraphicsContextSkia.cpp', raster_context)
    patch(graphics + 'GraphicsContextSkia.h', lambda t: t.replace(
        '    SkiaImageToFenceMap m_imageToFenceMap;\n', '').replace(
        '    std::unique_ptr<SkiaImageAtlasLayoutBuilder> m_atlasLayoutBuilder;\n', ''))
    patch(graphics + 'SkiaRecordingResult.h', lambda t: t[:t.index('#pragma once')] +
          '#pragma once\nnamespace WebCore {\n'
          '// CPU canvas recording has no GPU atlas ownership or synchronization fences.\n'
          'struct SkiaRecordingData { };\n}\n')

    def raster_native_image(t):
        t = t.replace('#include "GLContext.h"\n', '').replace('#include "PlatformDisplay.h"\n', '')
        start = t.index('    if (platformImage->isTextureBacked()) {')
        end = t.index('\n    SkPixmap pixmap;', start)
        return t[:start] + '    RELEASE_ASSERT(!platformImage->isTextureBacked());\n' + t[end:]

    patch(graphics + 'NativeImageSkia.cpp', raster_native_image)

    def raster_encoding(t):
        t = t.replace('#include "GLContext.h"\n', '').replace('#include "PlatformDisplay.h"\n', '')

        # The software encoder below reads actual raster pixmaps.
        start = t.index('static sk_sp<SkData> encodeAcceleratedImage')
        end = t.index('\n}', start) + 2
        t = t[:start] + t[end:]
        start = t.index(
            '    if (image->isTextureBacked()) {', t.index('Vector<uint8_t> platformEncodeData'))
        end = t.index('\n    SkPixmap pixmap;', start)
        return t[:start] + '    RELEASE_ASSERT(!image->isTextureBacked());\n' + t[end:]

    patch(graphics + 'ImageUtilitiesSkia.cpp', raster_encoding)

    def raster_buffer(t):
        t = replace(t, '#if USE(SKIA)\n#include "GLContext.h"',
                    '#if USE(SKIA) && !MODERNOS_RASTER_ONLY\n#include "GLContext.h"')
        t = replace(t, '#elif USE(SKIA)\n        if (auto imageBuffer',
                    '#elif USE(SKIA) && !MODERNOS_RASTER_ONLY\n        if (auto imageBuffer')
        t = replace(t, '#if !USE(SKIA)', '#if !USE(SKIA) || MODERNOS_RASTER_ONLY')
        t = replace(t, '#if USE(SKIA)\n    return makeUnique<SkiaSerializedImageBuffer>',
                    '#if USE(SKIA) && !MODERNOS_RASTER_ONLY\n    return makeUnique<SkiaSerializedImageBuffer>')
        return t

    patch('Source/WebCore/platform/graphics/ImageBuffer.cpp', raster_buffer)

    # Preserve upstream's complete blend-mode mapping and license. Texture
    # promises are unnecessary for the genuine raster images used here.
    utilities = (SOURCE / (graphics + 'SkiaUtilities.cpp')).read_text()
    start = utilities.index('SkBlendMode toSkiaBlendMode(')
    end = utilities.index('\nclass PromiseImageContext', start)
    license_text = utilities[:utilities.index('#include "config.h"')]
    path = SOURCE / (graphics + 'SkiaRasterUtilities.cpp')
    write_if_changed(path, license_text + '#include "config.h"\n#include "SkiaUtilities.h"\n'
                     '#include "GraphicsTypes.h"\n#include "NotImplemented.h"\n#include <skia/core/SkImage.h>\n'
                     'namespace WebCore { namespace SkiaUtilities {\n' + utilities[start:end] +
                     '\nsk_sp<SkImage> createPromiseImageIfNeeded(const sk_sp<SkImage>& image, const sk_sp<GrContextThreadSafeProxy>& context)\n'
                     '{\n    RELEASE_ASSERT(!context && !image->isTextureBacked());\n    return image;\n}\n}}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--target', default='WebCore')
    parser.add_argument('--interpreter', choices=('cloop', 'llint'), default='llint',
                        help='Portable C interpreter or static x86-64 LLInt; JIT remains disabled')
    args = parser.parse_args()

    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be 1..32')
    prepare()
    notices()
    env = environment()
    libgcc = build_posix_libgcc(BUILD, PREFIX, args.jobs, env)
    runtime_cmake = BUILD / 'posix-runtime.cmake'
    runtime_cmake.write_text(f'link_libraries("{libgcc}")\n')
    env['PATH'] = str(BUILD / 'host-tools/ruby/bin') + ':' + env['PATH']
    toolchain = BUILD / 'webcore-toolchain.cmake'
    toolchain.write_text(f'''set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "{CROSS}gcc")
set(CMAKE_CXX_COMPILER "{BUILD}/musl-posix-g++")
set(CMAKE_C_FLAGS_INIT "{FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "{FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -no-pie")
set(CMAKE_FIND_ROOT_PATH "{PREFIX};{WEB_DEPS};{ROOT}/build/compiler/sysroot")
set(CMAKE_PREFIX_PATH "{PREFIX};{WEB_DEPS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
''')
    cmake = BUILD / 'host-tools/cmake-4.4.4-linux-x86_64/bin/cmake'
    directory = BUILD / ('webcore' if args.interpreter == 'cloop' else 'webcore-llint')
    log_prefix = 'webcore' if args.interpreter == 'cloop' else 'webcore-llint'
    run(BUILD, log_prefix + '-configure', [cmake, '-S', SOURCE, '-B', directory,
        '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={BUILD}/host-tools/ninja',
                                           f'-DCMAKE_TOOLCHAIN_FILE={toolchain}', '-DCMAKE_BUILD_TYPE=Release',
                                           f'-DCMAKE_PROJECT_INCLUDE={runtime_cmake}',
                                           '-DPORT=ModernOS', '-DDEVELOPER_MODE=OFF', '-DUSE_SYSTEM_MALLOC=ON',
                                           f'-DMODERNOS_PORT_DIR={PORT}',
                                           '-DENABLE_JIT=OFF', '-DENABLE_C_LOOP=' + ('ON' if args.interpreter == 'cloop' else 'OFF'),
                                           '-DENABLE_WEBASSEMBLY=' + ('OFF' if args.interpreter == 'cloop' else 'ON'),
                                           '-DENABLE_WEBASSEMBLY_BBQJIT=OFF', '-DENABLE_WEBASSEMBLY_OMGJIT=OFF',
                                           '-DENABLE_XSLT=OFF', '-DENABLE_VIDEO=ON', '-DENABLE_MEDIA_SOURCE=ON',
                                           '-DUSE_LCMS=OFF', '-DUSE_AVIF=OFF', '-DUSE_JPEGXL=OFF',
                                           f'-DJPEG_LIBRARY={PREFIX}/lib/libjpeg.a', f'-DJPEG_INCLUDE_DIR={PREFIX}/include',
                                           '-DENABLE_SAMPLING_PROFILER=OFF', f'-DICU_ROOT={PREFIX}',
                                           f'-DCMAKE_INSTALL_PREFIX={PREFIX}', '-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON',
                                           f'-DRuby_EXECUTABLE={BUILD}/host-tools/ruby/bin/ruby'], env)

    if not args.configure_only:
        run(BUILD, log_prefix + '-build', [cmake, '--build', directory, '--target', args.target,
            '--parallel', str(args.jobs)], env)
        if args.target == 'ModernOSRenderer':
            destination = ROOT / 'userland/bin/web-renderer'
            original = BUILD / 'webcore-original/userland/bin/web-renderer'
            if destination.exists() and not original.exists():
                original.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(destination, original)
            shutil.copy2(directory / 'bin/web-renderer', destination)

        print(f'Built genuine {args.target} for the ModernOS software browser.', flush=True)


if __name__ == '__main__':
    main()
