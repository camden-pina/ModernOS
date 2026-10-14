// Real HTTP(S) resource delivery through an origin-bound broker channel.
// SubresourceLoader retains WebCore's actual CSP/CORS/preflight/redirect checks.
#pragma once

#include "FixtureStrategies.h"
#include "HTTPParsers.h"
#include "SecurityOrigin.h"
#include "TimingAllowOrigin.h"
#include "Document.h"
#include "DocumentLoader.h"
#include "LocalFrame.h"
#include "../web-resource-io.h"
#include "../browser-control.h"
#include "../web-frame-io.h"
#include <wtf/Language.h>
#include <glib-unix.h>
#include <deque>
#include <string>
#include <functional>
#include <poll.h>

namespace ModernOS {
class BrokerLoader final : public FixtureLoader {
    WTF_MAKE_TZONE_ALLOCATED(BrokerLoader);

  public:
    using Control = std::function<void(const browser_packet &, const char *)>;
    using FrameControl =
        std::function<void(const web_frame_packet &, std::string, RefPtr<SharedBuffer>)>;

    void setFrameControl(FrameControl control) {
        m_frameControl = std::move(control);
    }

    explicit BrokerLoader(URL origin, Control control = {})
        : m_origin(WTF::move(origin)), m_control(std::move(control)) {
        m_source = g_unix_fd_add(
            0, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
            [](gint, GIOCondition, gpointer self) -> gboolean {
                auto &loader = *static_cast<BrokerLoader *>(self);
                loader.receive();
                return loader.m_broken ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
            },
            this);
    }

    ~BrokerLoader() override {
        if (m_source && !m_broken)
            g_source_remove(m_source);
    }

    bool isOnLine() const override {
        return !m_broken;
    }

    struct ServiceResult {
        uint32_t status = EIO, flags = 0;
        Vector<uint8_t> bytes;
    };

    ServiceResult service(unsigned operation, const URL &url, std::span<const uint8_t> key = {},
                          std::span<const uint8_t> value = {}, bool session = false) {
        ServiceResult result;
        auto encoded = url.string().utf8();
        if (m_broken || m_waiting || !m_next ||
            encoded.length() + key.size() > WEB_RESOURCE_MAX_METADATA ||
            value.size() > WEB_SERVICE_STORAGE_QUOTA)
            return result;

        std::string metadata(encoded.data(), encoded.length());
        if (!key.empty())
            metadata.append(reinterpret_cast<const char *>(key.data()), key.size());

        web_resource_packet packet = {};
        init(packet, WEB_SERVICE_REQUEST, m_next++);
        packet.status = operation;
        packet.flags = session ? WEB_SERVICE_SESSION : 0;
        packet.url_bytes = encoded.length();
        packet.headers_bytes = key.size();
        packet.metadata_bytes = metadata.size();
        packet.body_bytes = value.size();

        int fd = value.empty() ? -1 : web_resource_body(value.data(), value.size());
        if (!value.empty() && fd < 0)
            return result;

        int sent = web_resource_send(0, &packet, metadata.data(), fd);
        if (fd >= 0)
            close(fd);
        if (sent < 0) {
            breakChannel();
            return result;
        }

        m_waiting = packet.id;
        auto deadline = MonotonicTime::now() + 5_s;
        while (!m_broken && !m_serviceReply && MonotonicTime::now() < deadline) {
            pollfd input = {0, POLLIN, 0};
            int ready = poll(&input, 1, 100);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0) {
                breakChannel();
                break;
            }
            if (ready)
                receive(false);
        }

        if (m_serviceReply) {
            auto reply = WTF::move(*m_serviceReply);
            m_serviceReply.reset();
            if (reply.metadata != std::string(encoded.data(), encoded.length()))
                breakChannel();
            else {
                result.status = reply.packet.status;
                result.flags = reply.packet.flags;
                if (reply.bytes)
                    result.bytes.append(reply.bytes->span());
            }
        } else if (!m_broken)
            breakChannel();

        m_waiting = 0;
        RunLoop::mainSingleton().dispatch([this] {
            deliverReady();
            sendQueued();
        });
        return result;
    }

    void crossOriginRedirectReceived(ResourceLoader *loader, const URL &url) override {
        if (!loader || loader->reachedTerminalState())
            return;
        if (!url.protocolIsInHTTPFamily() ||
            (loader->cachedResource()->type() == CachedResource::Type::MainResource &&
             !SecurityOrigin::create(url)->isSameOriginAs(SecurityOrigin::create(m_origin))))
            loader->didFail(failure(
                url, "Redirect requires another origin renderer or unsupported protocol"_s));
    }

    void remove(ResourceLoader *loader) override {
        FixtureLoader::remove(loader);
        m_deferred.erase(loader);
        for (auto it = m_requests.begin(); it != m_requests.end();) {
            if (it->second.loader.get() != loader) {
                ++it;
                continue;
            }
            auto id = it->first;
            if (it->second.active) {
                auto url = loader->request().url().string().utf8();
                web_resource_packet p = {};
                init(p, WEB_RESOURCE_CANCEL, id);
                p.url_bytes = p.metadata_bytes = url.length();
                if (!m_broken)
                    web_resource_send(0, &p, url.data(), -1);
                --m_active;
            }

            m_received.erase(id);
            it = m_requests.erase(it);
        }

        m_redirects.erase(loader);
        RunLoop::mainSingleton().dispatch([this] { sendQueued(); });
    }

    void setDefersLoading(ResourceLoader &loader, bool defers) override {
        FixtureLoader::setDefersLoading(loader, defers);
        if (defers)
            m_deferred.insert(&loader);
        else {
            m_deferred.erase(&loader);
            RunLoop::mainSingleton().dispatch([this] {
                deliverReady();
                sendQueued();
            });
        }
    }

  private:
    struct Request {
        RefPtr<SubresourceLoader> loader;
        bool active = false;
    };

    struct Response {
        web_resource_packet packet;
        std::string metadata;
        RefPtr<SharedBuffer> bytes;
    };

    URL m_origin;
    Control m_control;
    FrameControl m_frameControl;
    guint m_source = 0;
    uint32_t m_next = 1;
    unsigned m_active = 0;
    uint32_t m_waiting = 0;
    std::optional<Response> m_serviceReply;
    bool m_broken = false;
    std::unordered_map<uint32_t, Request> m_requests;
    std::unordered_map<uint32_t, Response> m_received;
    struct RedirectState {
        unsigned count = 0;
        bool failsTAO = false, crossOrigin = false;
        MonotonicTime start;
    };

    std::unordered_map<ResourceLoader *, RedirectState> m_redirects;
    std::unordered_set<ResourceLoader *> m_deferred;
    std::deque<uint32_t> m_queued;

    static void init(web_resource_packet &p, unsigned type, uint32_t id) {
        p.magic = WEB_RESOURCE_MAGIC;
        p.version = WEB_RESOURCE_VERSION;
        p.type = type;
        p.id = id;
    }

    static ResourceError failure(const URL &url, const String &message) {
        return {"ModernOSBroker"_s, 1, url, message};
    }

    void loadNetwork(SubresourceLoader &loader) override {
        const auto &url = loader.request().url();
        if (m_broken || !url.protocolIsInHTTPFamily()) {
            loader.didFail(failure(url, "Resource protocol unavailable"_s));
            return;
        }
        if (loader.cachedResource()->type() == CachedResource::Type::MainResource &&
            !SecurityOrigin::create(url)->isSameOriginAs(SecurityOrigin::create(m_origin))) {
            loader.didFail(failure(url, "Cross-origin documents require a new renderer"_s));
            return;
        }
        if (m_requests.size() >= 128 || !m_next) {
            loader.didFail(failure(url, "Resource request quota exceeded"_s));
            return;
        }
        uint32_t id = m_next++;
        m_requests.emplace(id, Request{&loader, false});
        m_queued.push_back(id);
        sendQueued();
    }

    void sendQueued() {
        while (!m_broken && m_active < WEB_RESOURCE_MAX_ACTIVE && !m_queued.empty()) {
            uint32_t id = m_queued.front();
            m_queued.pop_front();
            auto it = m_requests.find(id);
            if (it == m_requests.end())
                continue;

            Ref loader = *it->second.loader;
            auto request = loader->request();
            // Use the same actual locale as Navigator, retaining an explicit
            // request value when a site supplies its own language preference.
            if (request.httpHeaderField(HTTPHeaderName::AcceptLanguage).isEmpty())
                request.setHTTPHeaderField(HTTPHeaderName::AcceptLanguage, defaultLanguage());

            auto url = request.url().string().utf8(), method = request.httpMethod().utf8();
            std::string metadata(url.data(), url.length());
            metadata.append(method.data(), method.length());
            for (const auto &header : request.httpHeaderFields()) {
                auto name = header.key.utf8(), value = header.value.utf8();
                metadata.append(name.data(), name.length());
                metadata.push_back(0);
                metadata.append(value.data(), value.length());
                metadata.push_back(0);
            }

            Vector<uint8_t> body;
            bool unsupportedBody = false;
            if (RefPtr data = request.httpBody()) {
                for (const auto &element : data->elements())
                    unsupportedBody |= !std::holds_alternative<Vector<uint8_t>>(element.data);
                if (!unsupportedBody)
                    body = data->flatten();
            }

            if (unsupportedBody || metadata.size() > WEB_RESOURCE_MAX_METADATA ||
                body.size() > WEB_RESOURCE_MAX_BODY) {
                loader->didFail(
                    failure(request.url(), "Resource metadata/body unavailable or over quota"_s));
                continue;
            }

            web_resource_packet p = {};
            init(p, WEB_RESOURCE_REQUEST, id);
            p.url_bytes = url.length();
            p.method_bytes = method.length();
            p.metadata_bytes = metadata.size();
            p.headers_bytes = p.metadata_bytes - p.url_bytes - p.method_bytes;
            p.body_bytes = body.size();
            p.flags = loader->cachedResource()->type() == CachedResource::Type::MainResource
                          ? WEB_RESOURCE_MAIN
                          : 0;

            // Include also applies to cross-origin CORS requests. WebCore
            // checks the response's origin/credentials policy; Soup selects
            // the destination's cookies without exposing them to this process.
            auto credentials = loader->options().credentials;
            if (request.allowCookies() &&
                (credentials == FetchOptions::Credentials::Include ||
                 (credentials == FetchOptions::Credentials::SameOrigin &&
                  SecurityOrigin::create(request.url())
                      ->isSameOriginAs(SecurityOrigin::create(m_origin)))))
                p.flags |= WEB_RESOURCE_CREDENTIALS;

            int fd = body.isEmpty() ? -1 : web_resource_body(body.span().data(), body.size());
            if (!body.isEmpty() && fd < 0) {
                loader->didFail(failure(request.url(), "Request body allocation failed"_s));
                continue;
            }

            int sent = web_resource_send(0, &p, metadata.data(), fd);
            if (fd >= 0)
                close(fd);
            if (sent < 0) {
                breakChannel();
                return;
            }

            it->second.active = true;
            ++m_active;
        }
    }

    void breakChannel() {
        m_broken = true;
        Vector<RefPtr<SubresourceLoader>> loaders;
        for (auto &[id, request] : m_requests)
            loaders.append(request.loader);
        m_requests.clear();
        m_received.clear();
        m_queued.clear();
        m_active = 0;
        for (auto &loader : loaders)
            if (!loader->reachedTerminalState())
                loader->didFail(failure(loader->request().url(), "Broker channel failed"_s));
    }

    void receive(bool deliver = true) {
        for (unsigned n = 0; n < 32; n++) {
            uint32_t magic = 0;
            ssize_t peek = recv(0, &magic, sizeof(magic), MSG_PEEK | MSG_DONTWAIT);
            if (peek < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (peek != (ssize_t)sizeof(magic)) {
                breakChannel();
                return;
            }
            if (magic == BROWSER_MAGIC && m_control) {
                browser_packet input = {};
                char text[BROWSER_MAX_TEXT + 1];
                if (browser_control_receive(0, &input, text) != 1) {
                    breakChannel();
                    return;
                }
                m_control(input, text);
                continue;
            }

            if (magic == WEB_FRAME_MAGIC && m_frameControl) {
                web_frame_packet packet = {};
                char metadata[WEB_FRAME_MAX_METADATA];
                int fd = -1;
                if (web_frame_receive(0, &packet, metadata, &fd, MSG_DONTWAIT) != 1) {
                    breakChannel();
                    return;
                }
                RefPtr<SharedBuffer> bytes;
                if (fd >= 0) {
                    void *mapped = mmap(nullptr, packet.body_bytes, PROT_READ, MAP_SHARED, fd, 0);
                    if (mapped == MAP_FAILED) {
                        close(fd);
                        breakChannel();
                        return;
                    }
                    bytes = SharedBuffer::create(
                        std::span{static_cast<const uint8_t *>(mapped), packet.body_bytes});
                    munmap(mapped, packet.body_bytes);
                    close(fd);
                }

                m_frameControl(packet, std::string(metadata, packet.metadata_bytes),
                               WTF::move(bytes));
                continue;
            }

            web_resource_packet packet = {};
            char metadata[WEB_RESOURCE_MAX_METADATA];
            int fd = -1;
            int result = web_resource_receive(0, &packet, metadata, &fd, MSG_DONTWAIT);
            if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (result <= 0) {
                breakChannel();
                return;
            }
            bool service = packet.type == WEB_SERVICE_RESPONSE;
            if ((!service && packet.type != WEB_RESOURCE_RESPONSE &&
                 packet.type != WEB_RESOURCE_ERROR) ||
                packet.method_bytes ||
                (service && (packet.id != m_waiting || m_serviceReply || packet.headers_bytes ||
                             packet.error_bytes || packet.flags & ~WEB_SERVICE_FOUND ||
                             packet.body_bytes > WEB_SERVICE_STORAGE_QUOTA)) ||
                (packet.type == WEB_RESOURCE_ERROR &&
                 (packet.body_bytes || packet.headers_bytes || packet.status))) {
                if (fd >= 0)
                    close(fd);
                breakChannel();
                return;
            }

            auto it = m_requests.find(packet.id);
            if (!service && it == m_requests.end()) {
                if (fd >= 0)
                    close(fd);
                continue;
            } // Cancelled before reply.
            if (!service && (!it->second.active || m_received.contains(packet.id))) {
                if (fd >= 0)
                    close(fd);
                breakChannel();
                return;
            }

            RefPtr<SharedBuffer> bytes;
            if (fd >= 0) {
                void *mapped = mmap(nullptr, packet.body_bytes, PROT_READ, MAP_SHARED, fd, 0);
                if (mapped == MAP_FAILED) {
                    close(fd);
                    if (service) {
                        breakChannel();
                        return;
                    }
                    Ref loader = *it->second.loader;
                    loader->didFail(
                        failure(loader->request().url(), "Response body mapping failed"_s));
                    continue;
                }

                bytes = SharedBuffer::create(
                    std::span{static_cast<const uint8_t *>(mapped), packet.body_bytes});
                munmap(mapped, packet.body_bytes);
                close(fd);
            }

            if (service) {
                m_serviceReply = Response{packet, std::string(metadata, packet.metadata_bytes),
                                          WTF::move(bytes)};
                continue;
            }
            m_received.emplace(
                packet.id,
                Response{packet, std::string(metadata, packet.metadata_bytes), WTF::move(bytes)});
        }

        if (deliver) {
            deliverReady();
            sendQueued();
        }
    }

    void deliverReady() {
        Vector<uint32_t> ready;
        for (const auto &[id, response] : m_received) {
            auto it = m_requests.find(id);
            if (it != m_requests.end() && !m_deferred.contains(it->second.loader.get()))
                ready.append(id);
        }
        for (uint32_t id : ready) {
            auto it = m_requests.find(id);
            if (it == m_requests.end())
                continue;
            Ref loader = *it->second.loader;
            Response reply = WTF::move(m_received.at(id));
            m_received.erase(id);
            m_requests.erase(it);
            --m_active;
            if (loader->reachedTerminalState())
                continue;
            const auto &p = reply.packet;
            URL url{String::fromUTF8(std::span{reply.metadata.data(), p.url_bytes})};
            if (!url.isValid() || url != loader->request().url()) {
                loader->didFail(failure(loader->request().url(), "Invalid broker response URL"_s));
                continue;
            }
            if (p.type == WEB_RESOURCE_ERROR) {
                loader->didFail(
                    failure(url, String::fromUTF8(std::span{reply.metadata.data() + p.url_bytes,
                                                            p.error_bytes})));
                continue;
            }

            if (p.flags & WEB_RESOURCE_DOWNLOAD) {
                ResourceError cancelled = failure(url, "Download completed"_s);
                cancelled.setType(ResourceError::Type::Cancellation);
                loader->didFail(cancelled);
                continue;
            }

            ResourceResponse response(URL(url), {}, p.body_bytes, {});
            response.setHTTPStatusCode(p.status);
            response.setSource(ResourceResponse::Source::Network);
            NetworkLoadMetrics metrics;
            const char *cursor = reply.metadata.data() + p.url_bytes,
                       *end = cursor + p.headers_bytes;
            bool valid = p.status >= 100 && p.status <= 599 && !p.error_bytes;
            while (valid && cursor < end) {
                const char *value = static_cast<const char *>(memchr(cursor, 0, end - cursor));
                const char *next =
                    value ? static_cast<const char *>(memchr(value + 1, 0, end - value - 1))
                          : nullptr;
                if (!next || value == cursor) {
                    valid = false;
                    break;
                }
                response.addHTTPHeaderField(
                    String::fromUTF8(std::span{cursor, static_cast<size_t>(value - cursor)}),
                    String::fromUTF8(std::span{value + 1, static_cast<size_t>(next - value - 1)}));
                cursor = next + 1;
            }

            if (!valid || response.containsInvalidHTTPHeaders()) {
                loader->didFail(failure(url, "Invalid broker response headers/status"_s));
                continue;
            }
            if (p.metrics_bytes) {
                web_network_metrics observed;
                memcpy(&observed, reply.metadata.data() + p.url_bytes + p.headers_bytes,
                       sizeof(observed));
                if (observed.reserved || observed.protocol < WEB_NETWORK_HTTP10 ||
                    observed.protocol > WEB_NETWORK_HTTP2 || !observed.fetch_start ||
                    observed.request_start < observed.fetch_start ||
                    observed.response_start < observed.request_start ||
                    (observed.response_end && observed.response_end < observed.response_start)) {
                    loader->didFail(failure(url, "Invalid broker response metrics"_s));
                    continue;
                }

                auto time = [](uint64_t us) {
                    return MonotonicTime::fromRawSeconds(static_cast<double>(us) / 1000000);
                };

                metrics.fetchStart = time(observed.fetch_start);
                metrics.domainLookupStart = time(observed.dns_start);
                metrics.domainLookupEnd = time(observed.dns_end);
                metrics.connectStart = time(observed.connect_start);
                metrics.secureConnectionStart = time(observed.tls_start);
                metrics.connectEnd = time(observed.connect_end);
                metrics.requestStart = time(observed.request_start);
                metrics.responseStart = time(observed.response_start);
                metrics.responseEnd = time(observed.response_end);
                metrics.responseBodyBytesReceived = observed.encoded_body_bytes;
                metrics.responseBodyDecodedSize = p.body_bytes;
                metrics.protocol = observed.protocol == WEB_NETWORK_HTTP2    ? "h2"_s
                                   : observed.protocol == WEB_NETWORK_HTTP11 ? "http/1.1"_s
                                                                             : "http/1.0"_s;
                if (observed.response_end)
                    metrics.markComplete();
            }

            auto &redirects = m_redirects[loader.ptr()];
            RefPtr<SecurityOrigin> source;
            if (loader->cachedResource()->type() == CachedResource::Type::MainResource)
                source = SecurityOrigin::create(loader->originalRequest().url());
            else if (loader->request().httpOrigin() == "null"_s)
                source = SecurityOrigin::createOpaque();
            else if (auto *origin = loader->cachedResource()->origin())
                source = origin;
            else if (RefPtr frame = loader->frame()) {
                if (RefPtr document = frame->document())
                    source = &document->securityOrigin();
            }
            redirects.failsTAO |= !source || !passesTimingAllowOriginCheck(response, *source);
            metrics.failsTAOCheck = redirects.failsTAO;
            metrics.redirectCount = redirects.count;
            metrics.redirectStart = redirects.start;
            metrics.hasCrossOriginRedirect = redirects.crossOrigin;
            if (p.metrics_bytes)
                response.setDeprecatedNetworkLoadMetrics(
                    Box<NetworkLoadMetrics>::create(metrics.isolatedCopy()));
            auto contentType = response.httpHeaderField(HTTPHeaderName::ContentType);
            response.setMimeType(extractMIMETypeFromMediaType(contentType));
            response.setTextEncodingName(extractCharsetFromMediaType(contentType).toString());
            auto location = response.httpHeaderField(HTTPHeaderName::Location);
            if (response.isRedirection() && !location.isEmpty() &&
                loader->options().redirect != FetchOptions::Redirect::Manual) {
                if (++redirects.count > 20) {
                    loader->didFail(failure(url, "Redirect limit exceeded"_s));
                    continue;
                }
                URL destination(url, location);
                redirects.crossOrigin |= !SecurityOrigin::create(url)->isSameOriginAs(
                    SecurityOrigin::create(destination));
                if (!redirects.start)
                    redirects.start = metrics.fetchStart;
                ResourceRequest redirected = loader->request();
                redirected.setURL(WTF::move(destination));
                loader->willSendRequest(WTF::move(redirected), response,
                                        [this, loader](ResourceRequest &&request) {
                                            if (loader->reachedTerminalState())
                                                return;
                                            if (request.isNull()) {
                                                loader->cancel();
                                                return;
                                            }
                                            loadNetwork(loader.get());
                                        });
            } else {
                // Preserve the genuine response policy check and terminal-state
                // guards, then finish with Soup's actual network measurements.
                loader->didReceiveResponse(
                    WTF::move(response), [loader, buffer = WTF::move(reply.bytes),
                                          metrics = WTF::move(metrics)]() mutable {
                        if (loader->reachedTerminalState())
                            return;
                        if (buffer) {
                            auto size = buffer->size();
                            loader->didReceiveBuffer(*buffer, size, DataPayloadWholeResource);
                            if (loader->reachedTerminalState())
                                return;
                        }

                        loader->didFinishLoading(metrics);
                    });
            }
        }
    }
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(BrokerLoader);

class BrokerStrategies final : public PlatformStrategies {
  public:
    explicit BrokerStrategies(URL origin, BrokerLoader::Control control = {})
        : m_loader(WTF::move(origin), std::move(control)) {
    }

    BrokerLoader &brokerLoader() {
        return m_loader;
    }

  private:
    LoaderStrategy *createLoaderStrategy() override {
        return &m_loader;
    }

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
    BrokerLoader m_loader;
    FixtureMedia m_media;
    FixtureBlobs m_blobs;
};
}
