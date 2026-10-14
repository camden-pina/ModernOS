// Real FrameTree mirrors for ancestors, siblings and foreign descendants.
// The owned local tree remains WebCore's authority. Broker updates only mirror
// structure; existing WindowProxy objects survive renames and reordering.
#include <algorithm>

struct NativeTreeEntry {
    uint64_t frame, parent;
    bool local;
    String name, origin;
};

static LocalFrame *nativeTreeRoot;
static std::vector<NativeTreeEntry> nativeLastOwnedTree;
static std::unordered_map<uint64_t, RefPtr<RemoteFrame>> nativeTreeMirrors;
static std::vector<uint8_t> nativeLastTreeBytes;
static uint64_t nativeOwnedTreeRevision;

static bool nativeDecodeTree(std::span<const uint8_t> bytes, std::vector<NativeTreeEntry> &result) {
    web_frame_tree_span spans[WEB_FRAME_TREE_MAX_NODES];
    uint32_t count;
    uint64_t root;
    if (!web_frame_tree_decode(bytes.data(), bytes.size(), spans, &count, &root))
        return false;
    for (unsigned index = 0; index < count; ++index) {
        const auto &span = spans[index];
        const auto &node = span.node;
        auto name = node.name_bytes ? String::fromUTF8(std::span(span.name, node.name_bytes))
                                    : emptyString();
        auto origin = String::fromUTF8(std::span(span.origin, node.origin_bytes));
        if (origin != "null"_s) {
            auto canonical = SecurityOrigin::createFromString(origin);
            if (canonical->isOpaque() || canonical->toString() != origin)
                return false;
        }
        result.push_back({node.frame, node.parent, !!(node.flags & WEB_FRAME_TREE_LOCAL),
                          WTF::move(name), WTF::move(origin)});
    }

    return true;
}

static void nativeEncodeTreeEntry(std::vector<uint8_t> &bytes, const NativeTreeEntry &entry) {
    auto name = entry.name.utf8(), origin = entry.origin.utf8();
    web_frame_tree_node node = {.frame = entry.frame,
                                .parent = entry.parent,
                                .flags = entry.local ? WEB_FRAME_TREE_LOCAL : 0U,
                                .name_bytes = static_cast<uint32_t>(name.length()),
                                .origin_bytes = static_cast<uint32_t>(origin.length())};
    auto old = bytes.size();
    bytes.resize(old + sizeof(node) + name.length() + origin.length());
    memcpy(bytes.data() + old, &node, sizeof(node));
    memcpy(bytes.data() + old + sizeof(node), name.data(), name.length());
    memcpy(bytes.data() + old + sizeof(node) + name.length(), origin.data(), origin.length());
}

static void nativeCollectOwnedTree(Frame &frame, uint64_t parent,
                                   std::vector<NativeTreeEntry> &entries, unsigned depth = 0) {
    if (entries.size() >= WEB_FRAME_TREE_MAX_NODES || depth > WEB_FRAME_TREE_MAX_DEPTH) {
        nativeQuit = true;
        return;
    }
    auto *local = dynamicDowncast<LocalFrame>(frame);
    String origin = local && local->document() ? local->document()->securityOrigin().toString()
                    : frame.frameTreeSyncData().frameDocumentSecurityOrigin
                        ? frame.frameTreeSyncData().frameDocumentSecurityOrigin->toString()
                        : "null"_s;
    auto name = frame.tree().specifiedName().string();
    if (name.utf8().length() > WEB_FRAME_TREE_MAX_NAME) {
        nativeQuit = true;
        return;
    }
    entries.push_back(
        {frame.frameID().toRawValue(), parent, !!local, WTF::move(name), WTF::move(origin)});
    // A remote frame's descendants belong to a different renderer. Mirrors
    // received from the broker cannot enlarge this renderer's owned snapshot.
    if (!local)
        return;
    for (RefPtr<Frame> child = frame.tree().firstChild(); child;
         child = child->tree().nextSibling())
        nativeCollectOwnedTree(*child, frame.frameID().toRawValue(), entries, depth + 1);
}

static void nativePublishOwnedTree() {
    if (!nativeTreeRoot || !nativeRootID || nativeQuit)
        return;
    std::vector<NativeTreeEntry> entries;
    nativeCollectOwnedTree(*nativeTreeRoot, 0, entries);
    if (nativeQuit || entries.empty())
        return;
    web_frame_tree_header header = {WEB_FRAME_TREE_MAGIC, WEB_FRAME_TREE_VERSION,
                                    static_cast<uint32_t>(entries.size()), 0, nativeRootID};
    std::vector<uint8_t> bytes(sizeof(header));
    memcpy(bytes.data(), &header, sizeof(header));
    for (const auto &entry : entries)
        nativeEncodeTreeEntry(bytes, entry);
    if (bytes.size() > WEB_FRAME_TREE_MAX_BYTES) {
        nativeQuit = true;
        return;
    }
    if (bytes == nativeLastTreeBytes)
        return;
    if (!++nativeOwnedTreeRevision) {
        nativeQuit = true;
        return;
    }
    nativeLastOwnedTree = std::move(entries);
    nativeLastTreeBytes = bytes;
    nativeFrameSend(
        {.type = WEB_FRAME_TREE, .frame = nativeRootID, .source = nativeOwnedTreeRevision}, {},
        bytes);
}

static RefPtr<Frame> nativeFindFrame(Page &page, uint64_t id) {
    for (RefPtr<Frame> frame = &page.mainFrame(); frame; frame = frame->tree().traverseNext())
        if (frame->frameID().toRawValue() == id)
            return frame;
    return nullptr;
}

static RefPtr<LocalFrame> nativeFindLocalFrame(Page &page, uint64_t id) {
    return dynamicDowncast<LocalFrame>(nativeFindFrame(page, id));
}

static bool nativeApplyTree(Page &page, const std::vector<NativeTreeEntry> &entries,
                            uint64_t acknowledged, bool bootstrap = false) {
    if (entries.empty() || entries.front().frame != page.mainFrame().frameID().toRawValue())
        return false;
    std::unordered_map<uint64_t, RefPtr<Frame>> frames;
    for (RefPtr<Frame> frame = &page.mainFrame(); frame; frame = frame->tree().traverseNext())
        frames.emplace(frame->frameID().toRawValue(), frame);
    std::unordered_map<uint64_t, const NativeTreeEntry *> wanted;
    for (const auto &entry : entries)
        wanted.emplace(entry.frame, &entry);
    // Remove only external mirror nodes. Real local descendants and their
    // actual embedded remote owners are never removed by a stale snapshot.
    std::vector<uint64_t> removed;
    for (const auto &[id, frame] : nativeTreeMirrors)
        if (!wanted.contains(id))
            removed.push_back(id);
    auto depth = [](const RefPtr<RemoteFrame> &frame) {
        unsigned value = 0;
        for (RefPtr<Frame> parent = frame->tree().parent(); parent;
             parent = parent->tree().parent())
            ++value;
        return value;
    };

    std::sort(removed.begin(), removed.end(), [&](uint64_t left, uint64_t right) {
        return depth(nativeTreeMirrors.at(left)) > depth(nativeTreeMirrors.at(right));
    });
    // Descendants detach before ancestors so parent/child links stay valid.
    for (auto id : removed) {
        auto found = nativeTreeMirrors.find(id);
        if (found == nativeTreeMirrors.end())
            continue;
        RefPtr frame = found->second;
        static_cast<Frame &>(*frame).frameDetached();
        frames.erase(id);
        nativeTreeMirrors.erase(found);
    }

    for (const auto &entry : entries) {
        RefPtr<Frame> frame;
        auto found = frames.find(entry.frame);
        if (found != frames.end())
            frame = found->second;
        else {
            if (entry.local)
                continue;
            auto parent = frames.find(entry.parent);
            if (parent == frames.end() || is<LocalFrame>(*parent->second))
                continue;
            Ref remote = RemoteFrame::createSubframe(
                page,
                [](RemoteFrame &frame) -> UniqueRef<RemoteFrameClient> {
                    return makeUniqueRef<NativeRemoteClient>(frame, true);
                },
                FrameIdentifier(entry.frame), *parent->second, nullptr, std::nullopt,
                FrameTreeSyncData::create(), Frame::AddToFrameTree::Yes);
            frame = remote.ptr();
            nativeTreeMirrors.emplace(entry.frame, remote.ptr());
            frames.emplace(entry.frame, frame);
        }

        if (auto *remote = dynamicDowncast<RemoteFrame>(frame.get()))
            remote->frameTreeSyncData().frameDocumentSecurityOrigin =
                entry.origin == "null"_s ? SecurityOrigin::createOpaque()
                                         : SecurityOrigin::createFromString(entry.origin);
        bool own = is<LocalFrame>(*frame) || (is<RemoteFrame>(*frame) && frame->ownerElement());
        bool changedLocally = own && acknowledged < nativeOwnedTreeRevision;
        if (own && !bootstrap)
            for (const auto &previous : nativeLastOwnedTree)
                if (previous.frame == entry.frame) {
                    changedLocally |= frame->tree().specifiedName().string() != previous.name;
                    break;
                }
        if (!changedLocally)
            frame->tree().setSpecifiedName(AtomString(entry.name));
    }

    // Rebuild only external parents' ordered child links. Frame and WindowProxy
    // instances are retained, including our local root in its true position.
    for (const auto &entry : entries) {
        auto found = frames.find(entry.frame);
        if (found == frames.end() || is<LocalFrame>(*found->second))
            continue;
        RefPtr<Frame> parent = found->second;
        std::vector<RefPtr<Frame>> children;
        for (const auto &child : entries)
            if (child.parent == entry.frame) {
                auto current = frames.find(child.frame);
                if (current != frames.end())
                    children.push_back(current->second);
            }
        for (RefPtr<Frame> child = parent->tree().firstChild(); child;) {
            RefPtr next = child->tree().nextSibling();
            parent->tree().removeChild(*child);
            child = next;
        }
        for (const auto &child : children) {
            if (RefPtr old = child->tree().parent())
                old->tree().removeChild(*child);
            parent->tree().appendChild(*child);
        }
    }

    return true;
}

static bool nativeReceiveTree(LocalFrame &local, const NativeFrameInput &input) {
    const auto &packet = input.packet;
    if (packet.flags != 1 || packet.frame != nativeRootID || packet.metadata_bytes ||
        !input.bytes || packet.x || packet.y || packet.width || packet.height ||
        packet.source > nativeOwnedTreeRevision)
        return false;
    std::vector<NativeTreeEntry> entries;
    return nativeDecodeTree(input.bytes->span(), entries) &&
           nativeApplyTree(*local.page(), entries, packet.source);
}
