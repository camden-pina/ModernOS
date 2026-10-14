// Controlled WebCore rendering fixture. HTTP(S) is explicitly unavailable here;
// a production broker LoaderStrategy must replace this before browsing sites.
#pragma once

#include "BlobRegistryImpl.h"
#include "CachedResource.h"
#include "LoaderStrategy.h"
#include "MediaStrategy.h"
#include "NetworkLoadMetrics.h"
#include "PlatformStrategies.h"
#include "PolicyContainer.h"
#include "ResourceError.h"
#include "ResourceLoader.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SubresourceLoader.h"
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>
#include <unordered_map>
#include <unordered_set>

namespace ModernOS {
using namespace WebCore;
using namespace WTF;

class FixtureLoader : public LoaderStrategy {
    WTF_MAKE_TZONE_ALLOCATED(FixtureLoader);

  public:
    void loadResource(LocalFrame &frame, CachedResource &resource, ResourceRequest &&request,
                      const ResourceLoaderOptions &options,
                      CompletionHandler<void(RefPtr<SubresourceLoader> &&)> &&done) override {
        SubresourceLoader::create(
            frame, resource, WTF::move(request), options,
            [this, done = WTF::move(done)](RefPtr<SubresourceLoader> &&loader) mutable {
                if (loader)
                    m_pending.emplace(loader.get(), loader);
                done(WTF::move(loader));
                RunLoop::mainSingleton().dispatch(
                    [this] { servePendingRequests(ResourceLoadPriority::VeryLow); });
            });
    }

    void loadResourceSynchronously(FrameLoader &, ResourceLoaderIdentifier,
                                   const ResourceRequest &request, ClientCredentialPolicy,
                                   const FetchOptions &, const HTTPHeaderMap &,
                                   ResourceError &failure, ResourceResponse &response,
                                   Vector<uint8_t> &bytes) override {
        failure = error(request.url(), 2, "Synchronous fixture network load is unavailable"_s);
        response = {};
        bytes.clear();
    }

    void pageLoadCompleted(Page &) override {
        servePendingRequests(ResourceLoadPriority::VeryLow);
    }

    void browsingContextRemoved(LocalFrame &frame) override {
        Vector<RefPtr<SubresourceLoader>> removed;
        for (auto &[key, loader] : m_pending) {
            if (loader->frame() == &frame)
                removed.append(loader);
        }
        for (auto &loader : removed)
            loader->cancel(cancelledError(loader->request()));
    }

    void remove(ResourceLoader *loader) override {
        m_pending.erase(loader);
        m_deferred.erase(loader);
    }

    void setDefersLoading(ResourceLoader &loader, bool defers) override {
        if (defers)
            m_deferred.insert(&loader);
        else {
            m_deferred.erase(&loader);
            RunLoop::mainSingleton().dispatch(
                [this] { servePendingRequests(ResourceLoadPriority::VeryLow); });
        }
    }

    void crossOriginRedirectReceived(ResourceLoader *loader, const URL &) override {
        if (loader && !loader->reachedTerminalState())
            loader->didFail(blockedError(loader->request()));
    }

    void servePendingRequests(ResourceLoadPriority) override {
        if (m_suspended)
            return;
        Vector<RefPtr<SubresourceLoader>> ready;
        for (auto &[key, loader] : m_pending) {
            if (!m_deferred.contains(key))
                ready.append(loader);
        }
        for (auto &loader : ready) {
            m_pending.erase(loader.get());
            if (loader->reachedTerminalState())
                continue;
            if (loader->request().url().protocolIsData() ||
                loader->request().url().protocolIsBlob())
                // Actual in-process DataURLDecoder / BlobRegistryImpl handles.
                // DocumentThreadableLoader retains CSP and origin checks;
                // HTTP(S) continues through the restricted network broker.
                loader->start();
            else
                loadNetwork(*loader);
        }
    }

    void suspendPendingRequests() override {
        m_suspended = true;
    }

    void resumePendingRequests() override {
        m_suspended = false;
        servePendingRequests(ResourceLoadPriority::VeryLow);
    }

    void startPingLoad(LocalFrame &, ResourceRequest &request, const HTTPHeaderMap &,
                       const FetchOptions &, ContentSecurityPolicyImposition,
                       PingLoadCompletionHandler &&done) override {
        if (done)
            done(blockedError(request), {});
    }

    void preconnectTo(FrameLoader &, ResourceRequest &&request, StoredCredentialsPolicy,
                      ShouldPreconnectAsFirstParty, PreconnectCompletionHandler &&done) override {
        done(blockedError(request));
    }

    void setCaptureExtraNetworkLoadMetricsEnabled(bool enabled) override {
        RELEASE_ASSERT(!enabled);
    }

    bool isOnLine() const override {
        return false;
    }

    void addOnlineStateChangeListener(Function<void(bool)> &&listener) override {
        m_onlineListeners.append(WTF::move(listener));
    }

    void isResourceLoadFinished(CachedResource &resource,
                                CompletionHandler<void(bool)> &&done) override {
        done(!resource.isLoading());
    }

    ResourceError cancelledError(const ResourceRequest &request) const override {
        auto failure = error(request.url(), 1, "Load cancelled"_s);
        failure.setType(ResourceError::Type::Cancellation);
        return failure;
    }

    ResourceError blockedError(const ResourceRequest &r) const override {
        return error(r.url(), 2, "Fixture load blocked"_s);
    }

    bool isBlockedError(const ResourceError &e) const override {
        return e.domain() == "ModernOSFixture"_s && e.errorCode() == 2;
    }

    ResourceError blockedByContentBlockerError(const ResourceRequest &r) const override {
        return error(r.url(), 3, "Content blocked"_s);
    }

    ResourceError cannotShowURLError(const ResourceRequest &r) const override {
        return error(r.url(), 4, "Unsupported URL"_s);
    }

    ResourceError interruptedForPolicyChangeError(const ResourceRequest &r) const override {
        return error(r.url(), 5, "Navigation policy changed"_s);
    }

#if ENABLE(CONTENT_FILTERING)
    ResourceError blockedByContentFilterError(const ResourceRequest &r) const override {
        return error(r.url(), 6, "Content filter blocked"_s);
    }

#endif
    ResourceError cannotShowMIMETypeError(const ResourceResponse &r) const override {
        return error(r.url(), 7, "Unsupported MIME type"_s);
    }

    ResourceError fileDoesNotExistError(const ResourceResponse &r) const override {
        return error(r.url(), 8, "File unavailable"_s);
    }

    ResourceError httpsUpgradeRedirectLoopError(const ResourceRequest &r) const override {
        return error(r.url(), 9, "HTTPS redirect loop"_s);
    }

    ResourceError httpNavigationWithHTTPSOnlyError(const ResourceRequest &r) const override {
        return error(r.url(), 10, "HTTPS required"_s);
    }

    bool isHttpNavigationWithHTTPSOnlyError(const ResourceError &e) const override {
        return e.domain() == "ModernOSFixture"_s && e.errorCode() == 10;
    }

    ResourceError pluginWillHandleLoadError(const ResourceResponse &r) const override {
        return error(r.url(), 11, "Plug-ins unavailable"_s);
    }

  protected:
    virtual void loadNetwork(SubresourceLoader &loader) {
        loader.didFail(error(loader.request().url(), 2, "Fixture has no HTTP resource broker"_s));
    }

  private:
    std::unordered_map<ResourceLoader *, RefPtr<SubresourceLoader>> m_pending;
    std::unordered_set<ResourceLoader *> m_deferred;
    Vector<Function<void(bool)>> m_onlineListeners;
    bool m_suspended{false};

    static ResourceError error(const URL &url, int code, const String &reason) {
        return {"ModernOSFixture"_s, code, url, reason};
    }
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(FixtureLoader);

class FixtureMedia final : public MediaStrategy {
    WTF_MAKE_TZONE_ALLOCATED(FixtureMedia);

  public:
    FixtureMedia() = default;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(FixtureMedia);

class FixtureBlobs final : public BlobRegistry {
    WTF_MAKE_TZONE_ALLOCATED(FixtureBlobs);

  public:
    void registerInternalFileBlobURL(const URL &url, Ref<BlobDataFileReference> &&file,
                                     const String &, const String &type) override {
        m_impl.registerInternalFileBlobURL(url, WTF::move(file), type);
    }

    void registerInternalBlobURL(const URL &url, Vector<BlobPart> &&data,
                                 const String &type) override {
        m_impl.registerInternalBlobURL(url, WTF::move(data), type);
    }

    void registerBlobURL(const URL &url, const URL &source, const PolicyContainer &policy,
                         const std::optional<SecurityOriginData> &origin) override {
        m_impl.registerBlobURL(url, source, policy, origin);
    }

    void registerInternalBlobURLOptionallyFileBacked(const URL &url, const URL &source,
                                                     RefPtr<BlobDataFileReference> &&file,
                                                     const String &type) override {
        m_impl.registerInternalBlobURLOptionallyFileBacked(url, source, WTF::move(file), type, {});
    }

    void registerInternalBlobURLForSlice(const URL &url, const URL &source, long long start,
                                         long long end, const String &type) override {
        m_impl.registerInternalBlobURLForSlice(url, source, start, end, type);
    }

    void unregisterBlobURL(const URL &url,
                           const std::optional<SecurityOriginData> &origin) override {
        m_impl.unregisterBlobURL(url, origin);
    }

    void registerBlobURLHandle(const URL &url,
                               const std::optional<SecurityOriginData> &origin) override {
        m_impl.registerBlobURLHandle(url, origin);
    }

    void unregisterBlobURLHandle(const URL &url,
                                 const std::optional<SecurityOriginData> &origin) override {
        m_impl.unregisterBlobURLHandle(url, origin);
    }

    String blobType(const URL &url) override {
        return m_impl.blobType(url);
    }

    unsigned long long blobSize(const URL &url) override {
        return m_impl.blobSize(url);
    }

    void writeBlobsToTemporaryFilesForIndexedDB(
        const Vector<String> &urls, CompletionHandler<void(Vector<String> &&)> &&done) override {
        m_impl.writeBlobsToTemporaryFilesForIndexedDB(urls, WTF::move(done));
    }

    BlobRegistryImpl *blobRegistryImpl() override {
        return &m_impl;
    }

  private:
    BlobRegistryImpl m_impl;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(FixtureBlobs);

class FixtureStrategies final : public PlatformStrategies {
    LoaderStrategy *createLoaderStrategy() override {
        return &m_loader;
    }

    // The controlled fixture has no clipboard service or permission grants.

    PasteboardStrategy *createPasteboardStrategy() override {
        return nullptr;
    }

    MediaStrategy *createMediaStrategy() override {
        return &m_media;
    }

    BlobRegistry *createBlobRegistry() override {
        return &m_blobs;
    }

#if ENABLE(DECLARATIVE_WEB_PUSH)
    PushStrategy *createPushStrategy() override {
        return nullptr;
    }

#endif
    FixtureLoader m_loader;
    FixtureMedia m_media;
    FixtureBlobs m_blobs;
};
} // namespace ModernOS
