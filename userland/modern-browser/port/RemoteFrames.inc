// WebCore RemoteFrame mirrors. Documents stay in immutable-origin processes;
// only bounded raster surfaces and structured-clone messages cross the broker.
class NativeRemoteClient;
static std::unordered_map<uint64_t, NativeRemoteClient *> nativeRemoteFrames;
struct NativeFrameInput {
    web_frame_packet packet;
    std::string metadata;
    RefPtr<SharedBuffer> bytes;
};

static std::deque<NativeFrameInput> nativeFrameInputs;
static uint64_t nativeFocusedRemote;
static uint64_t nativeRootID;
static bool nativeReceiveTree(LocalFrame &, const NativeFrameInput &);
static RefPtr<LocalFrame> nativeFindLocalFrame(Page &, uint64_t);
static void nativeFocusRemote(RemoteFrame &);
static bool nativeFrameSend(web_frame_packet packet, std::string metadata = {},
                            std::span<const uint8_t> bytes = {}) {
    packet.metadata_bytes = metadata.size();
    packet.body_bytes = bytes.size();
    int fd = bytes.empty() ? -1 : web_resource_body(bytes.data(), bytes.size());
    if ((!bytes.empty() && fd < 0) || web_frame_send(0, packet, metadata.data(), fd) < 0)
        nativeQuit = true;
    if (fd >= 0)
        close(fd);
    return !nativeQuit;
}

static std::string nativeFrameRequest(const ResourceRequest &request) {
    auto url = request.url().string().utf8(), referrer = request.httpReferrer().utf8();
    std::string metadata(url.data(), url.length());
    metadata.push_back(0);
    metadata.append(referrer.data(), referrer.length());
    return metadata;
}

class NativeRemoteClient final : public RemoteFrameClient {
  public:
    explicit NativeRemoteClient(RemoteFrame &frame, bool parent = false)
        : m_frame(frame), m_parent(parent) {
        nativeRemoteFrames.emplace(frame.frameID().toRawValue(), this);
    }

    ~NativeRemoteClient() final {
        nativeRemoteFrames.erase(m_frame.frameID().toRawValue());
        if (!m_parent && !m_detached && !nativeQuit)
            nativeFrameSend({.type = WEB_FRAME_CLOSE, .frame = m_frame.frameID().toRawValue()});
    }

    RemoteFrame &frame() {
        return m_frame;
    }

    void sandbox(SandboxFlags flags) {
        m_sandbox = flags;
    }

    void image(RefPtr<NativeImage> &&image) {
        m_image = WTF::move(image);
        viewState.invalidateAll();
    }

    void frameDetached() final {
        if (m_detached)
            return;
        Ref protectedFrame{m_frame};
        RefPtr owner = m_frame.ownerElement();
        m_detached = true;
        nativeRemoteFrames.erase(m_frame.frameID().toRawValue());
        if (!m_parent && !nativeQuit)
            nativeFrameSend({.type = WEB_FRAME_CLOSE, .frame = m_frame.frameID().toRawValue()});
        if (nativeFocusedRemote == m_frame.frameID().toRawValue())
            nativeFocusedRemote = 0;
        if (RefPtr parent = m_frame.tree().parent()) {
            m_frame.tree().detachFromParent();
            parent->tree().removeChild(m_frame);
        }
        m_frame.disconnectOwnerElement();
        m_frame.detachFromPage();
        if (owner)
            owner->document().checkCompleted();
    }

    void frameRectDidChange(IntRect rect) final {
        if (m_parent || m_detached)
            return;
        int width = std::clamp(rect.width(), 1, 4096), height = std::clamp(rect.height(), 1, 4096);
        if (static_cast<uint64_t>(width) * height > BROWSER_MAX_PIXELS)
            return;
        if (m_width == width && m_height == height)
            return;
        m_width = width;
        m_height = height;
        nativeFrameSend({.type = WEB_FRAME_RESIZE,
                         .frame = m_frame.frameID().toRawValue(),
                         .width = width,
                         .height = height});
    }

    void paintContents(GraphicsContext &context, const IntRect &rect) final {
        if (!m_image)
            return;
        context.save();
        context.clip(rect);
        auto size = m_frame.view()->size();
        context.drawNativeImage(*m_image, FloatRect({0, 0}, size),
                                FloatRect({0, 0}, m_image->size()));
        context.restore();
    }

    void postMessageToRemote(FrameIdentifier source, const SecurityOriginData &sourceOrigin,
                             FrameIdentifier target, std::optional<SecurityOriginData> targetOrigin,
                             const MessageWithMessagePorts &message,
                             const std::optional<UserGestureTokenData> &) final {
        if (!message.message || !message.transferredPorts.isEmpty() ||
            message.message->hasBlobURLs() || message.message->nonSerializedDataToken())
            return;
        // A default "/" target from an opaque document cannot match a
        // distinct foreign document. Opaque identities are never replaced by
        // a shared "null" target; wildcard delivery remains explicitly chosen.
        if (targetOrigin && targetOrigin->isOpaque())
            return;
        nativePublishOwnedTree();
        // SecurityOriginData's tuple serializer returns an empty string for
        // opaque data. SecurityOrigin supplies its real HTML serialization,
        // "null", while preserving the originating opaque SecurityOriginData.
        auto from = SecurityOrigin::create(sourceOrigin.isolatedCopy())->toString().utf8();
        auto to = targetOrigin
                      ? SecurityOrigin::create(targetOrigin->isolatedCopy())->toString().utf8()
                      : CString("*");
        std::string metadata(from.data(), from.length());
        metadata.push_back(0);
        metadata.append(to.data(), to.length());
        nativeFrameSend({.type = WEB_FRAME_MESSAGE,
                         .frame = target.toRawValue(),
                         .source = source.toRawValue()},
                        std::move(metadata), message.message->wireBytes().span());
    }

    void changeLocation(FrameLoadRequest &&request,
                        std::optional<PrivateClickMeasurement> &&) final {
        if (m_parent)
            return;
        if (!request.resourceRequest().url().protocolIsInHTTPFamily() ||
            request.resourceRequest().httpMethod() != "GET"_s)
            return;
        nativePublishOwnedTree();
        RefPtr parent = m_frame.tree().parent();
        if (!parent || !is<LocalFrame>(*parent))
            return;
        nativeFrameSend({.type = WEB_FRAME_CREATE,
                         .flags = m_sandbox.toRaw(),
                         .frame = m_frame.frameID().toRawValue(),
                         .source = parent->frameID().toRawValue(),
                         .width = std::max(1, m_width),
                         .height = std::max(1, m_height)},
                        nativeFrameRequest(request.resourceRequest()));
    }

    void dispatchDecidePolicyForNavigationAction(
        const NavigationAction &, const ResourceRequest &, const ResourceResponse &, FormState *,
        const String &, std::optional<NavigationIdentifier>, std::optional<HitTestResult> &&, bool,
        NavigationUpgradeToHTTPSBehavior, SandboxFlags, PolicyDecisionMode,
        FramePolicyFunction &&done) final {
        done(PolicyAction::Ignore);
    }

    void updateSandboxFlags(SandboxFlags flags) final {
        m_sandbox = flags;
    }

    void updateOpener(std::optional<FrameIdentifier>) final {
    }

    void setPrinting(bool, FloatSize, FloatSize, float, AdjustViewSize) final {
    }

    String renderTreeAsText(size_t, OptionSet<RenderAsTextFlag>) final {
        return {};
    }

    String layerTreeAsText(size_t, OptionSet<LayerTreeAsTextOptions>) final {
        return {};
    }

    void closePage() final {
    }

    void bindRemoteAccessibilityFrames(
        int, FrameIdentifier, AccessibilityRemoteToken,
        CompletionHandler<void(AccessibilityRemoteToken, int)> &&done) final {
        done({}, 0);
    }

    void unbindRemoteAccessibilityFrames(int) final {
    }

    void updateRemoteFrameAccessibilityOffset(FrameIdentifier, IntPoint) final {
    }

#if ENABLE(ACCESSIBILITY_LOCAL_FRAME)
    void updateRemoteFrameAccessibilityInheritedState(FrameIdentifier,
                                                      const InheritedFrameState &) final {
    }

#endif
    void focus() final {
        if (!m_parent)
            nativeFocusRemote(m_frame);
    }

    void unfocus() final {
        if (nativeFocusedRemote == m_frame.frameID().toRawValue())
            nativeFocusedRemote = 0;
    }

    void updateScrollingMode(ScrollbarMode) final {
    }

    void reportMixedContentViolation(bool, const URL &) final {
    }

    void addResourceTimingFromChild(ResourceTiming &&) final {
    }

    void findFocusableElementDescendingIntoRemoteFrame(
        FocusDirection, const FocusEventData &, ShouldFocusElement,
        CompletionHandler<void(FoundElementInRemoteFrame)> &&done) final {
        done(FoundElementInRemoteFrame::No);
    }

    void findFocusableElementContinuingFromFrame(FocusDirection, FrameIdentifier,
                                                 const FocusEventData &, ShouldFocusElement) final {
    }

    void dispatchCrossOriginBeforeUnloadCheck(const SecurityOriginData &) final {
    }

  private:
    RemoteFrame &m_frame;
    bool m_parent = false, m_detached = false;
    int m_width = 0, m_height = 0;
    RefPtr<NativeImage> m_image;
    SandboxFlags m_sandbox;
};

static void nativeFocusChanged(Frame *frame) {
    if (!frame || !nativeRootID)
        return;
    if (auto *remote = dynamicDowncast<RemoteFrame>(frame)) {
        if (!remote->ownerElement())
            return; // The remote ancestor is outside this subtree.
    }
    nativeFrameSend(
        {.type = WEB_FRAME_FOCUS,
         .frame = nativeRootID,
         .source = is<LocalFrame>(frame) ? nativeRootID : frame->frameID().toRawValue()});
}

static void nativeNotifyActivation(LocalFrame &frame, MonotonicTime time) {
    if (!nativeRootID || !frame.document())
        return;
    double seconds = time.secondsSinceEpoch().value();
    uint64_t bits;
    static_assert(sizeof(bits) == sizeof(seconds));
    memcpy(&bits, &seconds, sizeof(bits));
    nativeFrameSend({.type = WEB_FRAME_ACTIVATE,
                     .flags = frame.document()->securityOrigin().isOpaque() ? 1U : 0U,
                     .frame = nativeRootID,
                     .source = bits});
}

static void nativeConsumeActivation() {
    if (nativeRootID)
        nativeFrameSend({.type = WEB_FRAME_CONSUME_ACTIVATION, .frame = nativeRootID});
}

static void nativeFocusRemote(RemoteFrame &remote) {
    RefPtr page = remote.page();
    if (!page)
        return;
    auto &focus = page->focusController();
    // Let WebCore clear the previous local element and derive activeElement
    // from the real iframe owner. Do not replace the DOM focus accessors.
    focus.setFocusedElement(nullptr, &remote, {}, BroadcastFocusedElement::No);
    focus.setFocusedFrame(&remote, BroadcastFocusedFrame::Yes);
    nativeFocusedRemote = remote.frameID().toRawValue();
}

static void nativeSwapToRemote(Ref<LocalFrame> local, ResourceRequest request) {
    RefPtr page = local->page();
    RefPtr owner = local->ownerElement();
    RefPtr parent = local->tree().parent();
    unsigned owned = 0;
    for (const auto &[id, client] : nativeRemoteFrames)
        if (client->frame().ownerElement())
            ++owned;
    if (!page || !owner || !parent || owned >= WEB_FRAME_MAX_CHILDREN)
        return;
    nativePublishOwnedTree();
    RefPtr renderer = local->ownerRenderer();
    auto sandbox = local->effectiveSandboxFlags();
    Ref remote = RemoteFrame::createSubframe(
        *page,
        [](RemoteFrame &frame) -> UniqueRef<RemoteFrameClient> {
            return makeUniqueRef<NativeRemoteClient>(frame);
        },
        local->frameID(), *parent, nullptr, std::nullopt, FrameTreeSyncData::create(),
        Frame::AddToFrameTree::No);
    static_cast<NativeRemoteClient &>(remote->client()).sandbox(sandbox);
    parent->tree().replaceChild(local, remote);
    local->disconnectOwnerElement();
    remote->setOwnerElement(owner.get());
    remote->takeWindowProxyAndOpenerFrom(local);
    remote->tree().setSpecifiedName(local->tree().specifiedName());
    if (renderer)
        renderer->setWidget(remote->view());
    if (page->focusController().focusedFrame() == local.ptr())
        page->focusController().setFocusedFrame(remote.ptr(), BroadcastFocusedFrame::No);
    owner->scheduleInvalidateStyleAndLayerComposition();
    local->loader().stopAllLoaders();
    auto rect = renderer ? snappedIntRect(renderer->contentBoxRect()) : IntRect(0, 0, 300, 150);
    nativeFrameSend({.type = WEB_FRAME_CREATE,
                     .flags = sandbox.toRaw(),
                     .frame = remote->frameID().toRawValue(),
                     .source = parent->frameID().toRawValue(),
                     .width = std::clamp(rect.width(), 1, 4096),
                     .height = std::clamp(rect.height(), 1, 4096)},
                    nativeFrameRequest(request));
    nativePublishOwnedTree();
    viewState.invalidateAll();
}

static void nativeFrameCommitted(LocalFrame &frame, unsigned flags) {
    nativePublishOwnedTree();
    if (!nativeEmbedding.enabled || !frame.isRootFrame())
        return;
    auto origin = frame.document()->securityOrigin().toString().utf8();
    nativeFrameSend({.type = WEB_FRAME_COMMITTED, .flags = flags, .frame = nativeEmbedding.frame},
                    std::string(origin.data(), origin.length()));
}

static void nativeRemotePointer(const RemoteUserInputEventData &data, const browser_packet &input) {
    if (!nativeRemoteFrames.contains(data.targetFrameID.toRawValue()))
        return;
    if (input.flags & 7)
        nativeFocusRemote(nativeRemoteFrames.at(data.targetFrameID.toRawValue())->frame());
    auto point = roundedIntPoint(data.transformedPoint);
    nativeFrameSend(
        {.type = WEB_FRAME_INPUT,
         .flags = input.flags & 7,
         .frame = data.targetFrameID.toRawValue(),
         .source = static_cast<uint64_t>(input.canvas_width) << 32 | input.canvas_height,
         .x = point.x(),
         .y = point.y(),
         .width = input.width,
         .height = 1});
}

static void nativeEvent(LocalFrame &, const browser_packet &, const char *);

static void nativeProcessFrames(LocalFrame &local) {
    while (!nativeFrameInputs.empty()) {
        auto input = std::move(nativeFrameInputs.front());
        nativeFrameInputs.pop_front();
        const auto &p = input.packet;
        if (p.type == WEB_FRAME_TREE) {
            if (!nativeReceiveTree(local, input))
                nativeQuit = true;
            continue;
        }
        auto mirror = nativeRemoteFrames.find(p.frame);
        if ((p.type == WEB_FRAME_ACTIVATE || p.type == WEB_FRAME_CONSUME_ACTIVATION) &&
            ((p.frame == nativeRootID && !p.flags) ||
             (p.type == WEB_FRAME_ACTIVATE && p.flags == 1 &&
              mirror != nativeRemoteFrames.end())) &&
            !p.metadata_bytes && !p.body_bytes && !p.x && !p.y && !p.width && !p.height) {
            double seconds;
            memcpy(&seconds, &p.source, sizeof(seconds));
            auto time = MonotonicTime::fromRawSeconds(seconds);
            if (p.flags == 1) {
                // Only the actual local ancestors of this remote child gain
                // activation. Unrelated same-origin local siblings do not.
                for (RefPtr<Frame> ancestor = mirror->second->frame().tree().parent(); ancestor;
                     ancestor = ancestor->tree().parent())
                    if (auto *frame = dynamicDowncast<LocalFrame>(ancestor.get());
                        frame && frame->window())
                        frame->window()->updateActivation(time);
                continue;
            }

            Ref origin{local.document()->securityOrigin()};
            for (RefPtr<Frame> current = &local; current;
                 current = current->tree().traverseNext(&local)) {
                auto *frame = dynamicDowncast<LocalFrame>(current.get());
                if (!frame || !frame->window())
                    continue;
                if (p.type == WEB_FRAME_CONSUME_ACTIVATION)
                    frame->window()->consumeLastActivationIfNecessary();
                else if (frame->document()->securityOrigin().isSameOriginAs(origin.get()))
                    frame->window()->updateActivation(time);
            }

            continue;
        }

        if (p.type == WEB_FRAME_FOCUS && p.frame == nativeRootID && !p.metadata_bytes &&
            !p.body_bytes && !p.flags && !p.x && !p.y && !p.width && !p.height) {
            auto &focus = local.page()->focusController();
            if (p.source == nativeRootID) {
                if (!focus.localFocusedFrame())
                    focus.setFocusedFrame(&local, BroadcastFocusedFrame::No);
                nativeFocusedRemote = 0;
            } else if (!p.source) {
                focus.setFocusedFrame(nullptr, BroadcastFocusedFrame::No);
                nativeFocusedRemote = 0;
            } else if (auto target = nativeRemoteFrames.find(p.source);
                       target != nativeRemoteFrames.end()) {
                auto &remote = target->second->frame();
                focus.setFocusedElement(nullptr, &remote, {}, BroadcastFocusedElement::No);
                focus.setFocusedFrame(&remote, BroadcastFocusedFrame::No);
                nativeFocusedRemote = p.source;
            }

            continue;
        }

        if (p.type == WEB_FRAME_INPUT && nativeEmbedding.enabled &&
            p.frame == nativeEmbedding.frame) {
            browser_packet event = {};
            event.type = p.height == 1 ? BROWSER_POINTER : BROWSER_KEY;
            event.x = p.x;
            event.y = p.y;
            event.width = p.width;
            event.flags = p.flags;
            event.canvas_width = p.source >> 32;
            event.canvas_height = static_cast<uint32_t>(p.source);
            nativeEvent(local, event, "");
            continue;
        }

        if (p.type == WEB_FRAME_MESSAGE && input.bytes) {
            RefPtr targetLocal = nativeFindLocalFrame(*local.page(), p.frame);
            if (!targetLocal || !targetLocal->document())
                continue;
            auto source = nativeRemoteFrames.find(p.source);
            if (source == nativeRemoteFrames.end())
                continue;
            auto separator = input.metadata.find('\0');
            if (separator == std::string::npos)
                continue;
            auto from = String::fromUTF8(std::span(input.metadata.data(), separator));
            auto to = String::fromUTF8(std::span(input.metadata.data() + separator + 1,
                                                 input.metadata.size() - separator - 1));
            Ref origin = from == "null"_s ? SecurityOrigin::createOpaque()
                                          : SecurityOrigin::createFromString(from);
            std::optional<SecurityOriginData> target;
            if (to != "*"_s)
                target = SecurityOrigin::createFromString(to)->data();
            auto *global = targetLocal->script().globalObject(mainThreadNormalWorldSingleton());
            if (!global)
                continue;
            Vector<uint8_t> wire;
            wire.append(input.bytes->span());
            MessageWithMessagePorts message{
                SerializedScriptValue::createFromWireBytes(WTF::move(wire)), {}};
            if (targetLocal->window())
                targetLocal->window()->postMessageFromRemoteFrame(
                    *global, RefPtr<WindowProxy>{&source->second->frame().windowProxy()},
                    origin->data(), WTF::move(target), message, std::nullopt);
            continue;
        }

        if (mirror == nativeRemoteFrames.end())
            continue;
        if (p.type == WEB_FRAME_SURFACE && input.bytes && p.width > 0 && p.height > 0 &&
            p.width <= 4096 && p.height <= 4096 &&
            static_cast<uint64_t>(p.width) * p.height <= BROWSER_MAX_PIXELS &&
            static_cast<uint64_t>(p.width) * p.height * 4 == input.bytes->size()) {
            auto pixels = input.bytes->span();
            auto data = SkData::MakeWithCopy(pixels.data(), pixels.size());
            auto image = SkImages::RasterFromData(SkImageInfo::MakeN32Premul(p.width, p.height),
                                                  data, p.width * 4);
            mirror->second->image(NativeImage::create(WTF::move(image)));
            nativeFrameSend({.type = WEB_FRAME_ACK, .frame = p.frame});
        } else if (p.type == WEB_FRAME_COMMITTED) {
            auto origin = String::fromUTF8(input.metadata);
            mirror->second->frame().frameTreeSyncData().frameDocumentSecurityOrigin =
                origin == "null"_s ? SecurityOrigin::createOpaque()
                                   : SecurityOrigin::createFromString(origin);
            if (p.flags & 1)
                static_cast<Frame &>(mirror->second->frame()).didFinishLoadInAnotherProcess();
            if (p.flags & 2)
                if (RefPtr owner = mirror->second->frame().ownerElement())
                    owner->dispatchEvent(Event::create(eventNames().loadEvent, Event::CanBubble::No,
                                                       Event::IsCancelable::No));
        }
    }
}

#include "NativeFrameTree.inc"
