// Software view invalidation and actual WebCore rendering-update scheduling.
#pragma once

#include "EmptyClients.h"
#include "PointerCharacteristics.h"
#include "Region.h"
#include "ScreenModernOS.h"
#include <vector>
#include <functional>
#include <stdio.h>

namespace ModernOS {
struct ViewState {
    int width = 320, height = 240;
    bool dirty = true, renderingRequired = false, repaintAll = true;
    struct ScrollCopy {
        WebCore::IntRect area, destination;
        WebCore::IntPoint source;
    };

    std::vector<ScrollCopy> scrollCopies;
    WebCore::Region damage;
    std::function<void(WebCore::Frame *)> focusedFrameChanged;

    void invalidateAll() {
        dirty = true;
        repaintAll = true;
        damage = {};
        scrollCopies.clear();
    }

    void invalidate(const WebCore::IntRect &rect) {
        dirty = true;
        if (repaintAll)
            return;
        damage.unite(WebCore::intersection(rect, {0, 0, width, height}));
        if (damage.gridSize() > 4096)
            invalidateAll();
    }

    void scroll(const WebCore::IntSize &delta, const WebCore::IntRect &rect,
                const WebCore::IntRect &clip) {
        dirty = true;
        if (repaintAll)
            return;
        auto area = WebCore::intersection(WebCore::intersection(rect, clip), {0, 0, width, height});
        if (area.isEmpty() || delta.isZero())
            return;
        auto destination = area;
        destination.move(delta);
        destination.intersect(area);
        if (destination.isEmpty()) {
            invalidate(area);
            return;
        }
        if (scrollCopies.size() >= 128) {
            invalidateAll();
            return;
        }
        // Pending damage moves with the backing pixels. Newly exposed strips
        // and WebCore's later fixed/sticky invalidations are painted normally.
        auto moved = damage;
        moved.intersect(area);
        moved.translate(delta);
        moved.intersect(area);
        damage.subtract(area);
        damage.unite(moved);
        WebCore::Region exposed(area);
        exposed.subtract(destination);
        damage.unite(exposed);
        // A burst of wheel ticks in one viewport needs one backing move.
        // Keep opposite directions and different clips as ordered operations.
        if (!scrollCopies.empty() && scrollCopies.back().area == area && !delta.width()) {
            auto &previous = scrollCopies.back();
            auto priorDelta = previous.destination.location() - previous.source;
            if (!priorDelta.width() &&
                static_cast<int64_t>(priorDelta.height()) * delta.height() > 0) {
                auto retained = previous.destination;
                retained.move(delta);
                retained.intersect(destination);
                if (retained.isEmpty())
                    scrollCopies.pop_back();
                else {
                    previous.destination = retained;
                    previous.source = retained.location() - (priorDelta + delta);
                }
            } else
                scrollCopies.push_back({area, destination, destination.location() - delta});
        } else
            scrollCopies.push_back({area, destination, destination.location() - delta});
        if (damage.gridSize() > 4096)
            invalidateAll();
    }
};

class ChromeModernOS final : public WebCore::EmptyChromeClient {
    WTF_MAKE_TZONE_ALLOCATED(ChromeModernOS);

  public:
    explicit ChromeModernOS(ViewState &state) : m_state(state) {
    }

  private:
    ViewState &m_state;

    bool isEmptyChromeClient() const override {
        return false;
    }

    WebCore::FloatRect windowRect() const override {
        if (windowWidth && windowHeight)
            return {static_cast<float>(windowX), static_cast<float>(windowY),
                    static_cast<float>(windowWidth), static_cast<float>(windowHeight)};
        return {0, 0, static_cast<float>(m_state.width), static_cast<float>(m_state.height)};
    }

    WebCore::FloatRect pageRect() const override {
        return {0, 0, static_cast<float>(m_state.width), static_cast<float>(m_state.height)};
    }

    void triggerRenderingUpdate() override {
        m_state.renderingRequired = true;
    }

    bool scheduleRenderingUpdate() override {
        m_state.renderingRequired = true;
        return true;
    }

    void invalidateRootView(const WebCore::IntRect &) override {
        m_state.dirty = true;
    }

    void invalidateContentsAndRootView(const WebCore::IntRect &rect) override {
        m_state.invalidate(rect);
    }

    void invalidateContentsForSlowScroll(const WebCore::IntRect &rect) override {
        m_state.invalidate(rect);
    }

    void scroll(const WebCore::IntSize &delta, const WebCore::IntRect &rect,
                const WebCore::IntRect &clip) override {
        m_state.scroll(delta, rect, clip);
    }

    void contentsSizeChanged(WebCore::LocalFrame &, const WebCore::IntSize &) const override {
        m_state.invalidateAll();
    }

    void focusedFrameChanged(WebCore::Frame *frame) override {
        if (m_state.focusedFrameChanged)
            m_state.focusedFrameChanged(frame);
    }

    WebCore::KeyboardUIMode keyboardUIMode() override {
        return WebCore::KeyboardAccessFull;
    }

    bool hoverSupportedByPrimaryPointingDevice() const override {
        return true;
    }

    bool hoverSupportedByAnyAvailablePointingDevice() const override {
        return true;
    }

    std::optional<WebCore::PointerCharacteristics>
    pointerCharacteristicsOfPrimaryPointingDevice() const override {
        return WebCore::PointerCharacteristics::Fine;
    }

    WTF::OptionSet<WebCore::PointerCharacteristics>
    pointerCharacteristicsOfAllAvailablePointingDevices() const override {
        return WebCore::PointerCharacteristics::Fine;
    }

    void addMessageToConsole(JSC::MessageSource, JSC::MessageLevel, const WTF::String &message,
                             unsigned line, unsigned, const WTF::String &url) override {
        fprintf(stderr, "WebCore console %s:%u: %s\n", url.utf8().data(), line,
                message.utf8().data());
    }
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(ChromeModernOS);
}
