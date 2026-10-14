// Origin-bound DOM cookies and Web Storage. Credentials and stored values
// live in the trusted broker; a renderer cannot choose another origin's store.
#pragma once

#include "BrokerLoader.h"
#include "CookieJar.h"
#include "Document.h"
#include "Storage.h"
#include "StorageArea.h"
#include "StorageEventDispatcher.h"
#include "StorageNamespace.h"
#include "StorageNamespaceProvider.h"
#include "StorageSessionProvider.h"
#include "StorageType.h"

namespace ModernOS {
class BrokerSessionProvider final : public StorageSessionProvider {
    // CookieJar's platform network session is never used: both DOM and HTTP
    // cookies are served by Soup in the broker, through overridden methods.
    NetworkStorageSession *storageSession() const override {
        return nullptr;
    }
};

class BrokerCookieJar final : public CookieJar {
  public:
    explicit BrokerCookieJar(BrokerLoader &loader)
        : CookieJar(adoptRef(*new BrokerSessionProvider)), m_loader(loader) {
    }

    String cookies(Document &, const URL &url) const override {
        auto reply = m_loader.service(WEB_SERVICE_COOKIE_GET, url);
        return reply.status ? String() : String::fromUTF8(reply.bytes.span());
    }

    void setCookies(Document &, const URL &url, const String &value) override {
        auto bytes = value.utf8();
        if (bytes.length() > 4096)
            return;
        m_loader.service(
            WEB_SERVICE_COOKIE_SET, url, {},
            std::span{reinterpret_cast<const uint8_t *>(bytes.data()), bytes.length()});
    }

    bool cookiesEnabled(Document &) override {
        return m_loader.isOnLine();
    }

    void remoteCookiesEnabled(const Document &,
                              CompletionHandler<void(bool)> &&done) const override {
        done(m_loader.isOnLine());
    }

    std::pair<String, SecureCookiesAccessed>
    cookieRequestHeaderFieldValue(const URL &, const SameSiteInfo &, const URL &,
                                  std::optional<FrameIdentifier>, std::optional<PageIdentifier>,
                                  IncludeSecureCookies) const override {
        // HTTP headers are attached by the broker, including HttpOnly cookies.
        return {String(), SecureCookiesAccessed::No};
    }

  private:
    BrokerLoader &m_loader;
};

static Vector<uint8_t> storageEncode(const String &value) {
    Vector<uint8_t> result;
    result.reserveInitialCapacity(value.length() * 2);
    for (unsigned i = 0; i < value.length(); i++) {
        auto c = value[i];
        result.append(c & 255);
        result.append(c >> 8);
    }
    return result;
}

static String storageDecode(const BrokerLoader::ServiceResult &reply) {
    if (reply.status || !(reply.flags & WEB_SERVICE_FOUND) || reply.bytes.size() % 2)
        return {};
    if (reply.bytes.isEmpty())
        return emptyString();
    Vector<char16_t> units;
    units.reserveInitialCapacity(reply.bytes.size() / 2);
    for (size_t i = 0; i < reply.bytes.size(); i += 2)
        units.append(reply.bytes[i] | (reply.bytes[i + 1] << 8));
    return String(units.span());
}

class BrokerStorageArea final : public StorageArea {
  public:
    BrokerStorageArea(BrokerLoader &loader, const SecurityOrigin &origin, StorageType type)
        : m_loader(loader), m_origin(origin.isolatedCopy()), m_url(origin.toString()),
          m_type(type) {
    }

    unsigned length() override {
        auto reply = call(WEB_SERVICE_STORAGE_LENGTH);
        uint32_t length = 0;
        if (!reply.status && reply.bytes.size() == sizeof(length))
            memcpy(&length, reply.bytes.span().data(), sizeof(length));
        return length;
    }

    String key(unsigned index) override {
        return storageDecode(
            call(WEB_SERVICE_STORAGE_KEY, {},
                 std::span{reinterpret_cast<const uint8_t *>(&index), sizeof(index)}));
    }

    String item(const String &key) override {
        auto bytes = storageEncode(key);
        return storageDecode(call(WEB_SERVICE_STORAGE_GET, bytes.span()));
    }

    bool contains(const String &key) override {
        return !item(key).isNull();
    }

    void setItem(LocalFrame &source, const String &key, const String &value,
                 bool &quotaException) override {
        quotaException = false;
        auto old = item(key);
        if (!old.isNull() && old == value)
            return;
        auto k = storageEncode(key), v = storageEncode(value);
        auto reply = call(WEB_SERVICE_STORAGE_SET, k.span(), v.span());
        if (reply.status) {
            quotaException = true;
            return;
        }
        changed(source, key, old, value);
    }

    void removeItem(LocalFrame &source, const String &key) override {
        auto old = item(key);
        if (old.isNull())
            return;
        auto bytes = storageEncode(key);
        if (!call(WEB_SERVICE_STORAGE_REMOVE, bytes.span()).status)
            changed(source, key, old, {});
    }

    void clear(LocalFrame &source) override {
        if (!length())
            return;
        if (!call(WEB_SERVICE_STORAGE_CLEAR).status)
            changed(source, {}, {}, {});
    }

    StorageType storageType() const override {
        return m_type;
    }

    size_t memoryBytesUsedByCache() override {
        return 0;
    }

  private:
    BrokerLoader &m_loader;
    Ref<SecurityOrigin> m_origin;
    URL m_url;
    StorageType m_type;
    BrokerLoader::ServiceResult call(unsigned operation, std::span<const uint8_t> key = {},
                                     std::span<const uint8_t> value = {}) {
        return m_loader.service(operation, m_url, key, value, m_type == StorageType::Session);
    }

    void changed(LocalFrame &source, const String &key, const String &old, const String &value) {
        if (!source.document())
            return;
        Function<bool(Storage &)> isSource = [window = source.window()](Storage &storage) {
            return storage.window() == window;
        };

        auto url = source.document()->url().string();
        if (m_type == StorageType::Session && source.page())
            StorageEventDispatcher::dispatchSessionStorageEvents(key, old, value, *source.page(),
                                                                 m_origin, url, isSource);
        else
            StorageEventDispatcher::dispatchLocalStorageEvents(key, old, value, nullptr, m_origin,
                                                               url, isSource);
    }
};

class BrokerStorageNamespace final : public StorageNamespace {
  public:
    BrokerStorageNamespace(BrokerLoader &loader, PAL::SessionID session, StorageType type)
        : m_loader(loader), m_session(session), m_type(type) {
    }

    Ref<StorageArea> storageArea(const SecurityOrigin &origin) override {
        auto key = origin.toString().utf8();
        auto it = m_areas.find(std::string(key.data(), key.length()));
        if (it != m_areas.end())
            return it->second.copyRef();
        Ref<StorageArea> area = adoptRef(*new BrokerStorageArea(m_loader, origin, m_type));
        m_areas.emplace(std::string(key.data(), key.length()), area.copyRef());
        return area;
    }

    const SecurityOrigin *topLevelOrigin() const override {
        return nullptr;
    }

    Ref<StorageNamespace> copy(Page &page) override {
        return adoptRef(*new BrokerStorageNamespace(m_loader, page.sessionID(), m_type));
    }

    PAL::SessionID sessionID() const override {
        return m_session;
    }

    void setSessionIDForTesting(PAL::SessionID session) override {
        m_session = session;
    }

  private:
    BrokerLoader &m_loader;
    PAL::SessionID m_session;
    StorageType m_type;
    std::unordered_map<std::string, Ref<StorageArea>> m_areas;
};

class BrokerStorageProvider final : public StorageNamespaceProvider {
  public:
    explicit BrokerStorageProvider(BrokerLoader &loader) : m_loader(loader) {
        setSessionStorageQuota(WEB_SERVICE_STORAGE_QUOTA);
    }

    RefPtr<StorageNamespace> sessionStorageNamespace(const SecurityOrigin &, Page &page,
                                                     ShouldCreateNamespace create) override {
        if (!m_session && create == ShouldCreateNamespace::Yes)
            m_session = adoptRef(
                *new BrokerStorageNamespace(m_loader, page.sessionID(), StorageType::Session));
        return m_session;
    }

  private:
    BrokerLoader &m_loader;
    RefPtr<StorageNamespace> m_session;

    Ref<StorageNamespace> createLocalStorageNamespace(unsigned, PAL::SessionID session) override {
        return adoptRef(*new BrokerStorageNamespace(m_loader, session, StorageType::Local));
    }

    Ref<StorageNamespace> createTransientLocalStorageNamespace(SecurityOrigin &, unsigned,
                                                               PAL::SessionID session) override {
        return adoptRef(
            *new BrokerStorageNamespace(m_loader, session, StorageType::TransientLocal));
    }
};
}
