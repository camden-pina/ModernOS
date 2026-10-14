// WebCore owns same-document state, form state and scroll restoration. The
// trusted broker keeps window history across immutable-origin processes.
#pragma once

#include "BackForwardClient.h"
#include "HistoryItem.h"
#include "ShouldTreatAsContinuingLoad.h"

namespace ModernOS {
class BrowserHistory final : public BackForwardClient {
  public:
    using Changed = Function<void(const String &, int, bool)>;
    explicit BrowserHistory(Changed &&changed) : m_changed(WTF::move(changed)) {
    }

    struct Entry {
        Ref<HistoryItem> item;
        int token;
    };

    int token() const {
        return m_items.empty() ? 0 : m_items[m_cursor].token;
    }

    bool takeTraversal() {
        return std::exchange(m_traversal, false);
    }

    RefPtr<HistoryItem> findToken(int token) {
        for (auto &entry : m_items)
            if (entry.token == token)
                return entry.item.ptr();
        return nullptr;
    }

    void addItem(Ref<HistoryItem> &&item) override {
        while (m_items.size() > m_cursor + 1)
            m_items.pop_back();
        if (m_items.size() == 128)
            m_items.erase(m_items.begin());
        m_items.push_back({WTF::move(item), m_next++});
        m_cursor = m_items.size() - 1;
        m_traversal = false;
        m_changed(m_items[m_cursor].item->urlString(), token(), false);
    }

    void setChildItem(BackForwardFrameItemIdentifier parent, Ref<HistoryItem> &&item) override {
        for (auto &entry : m_items)
            if (auto *found = findFrameItem(entry.item, parent)) {
                found->setChildItem(WTF::move(item));
                return;
            }
    }

    void goToItem(HistoryItem &item) override {
        for (size_t i = 0; i < m_items.size(); i++)
            if (m_items[i].item->itemID() == item.itemID()) {
                m_traversal = i != m_cursor;
                m_cursor = i;
                m_changed(m_items[m_cursor].item->urlString(), token(), m_traversal);
                return;
            }
    }

    Vector<Ref<HistoryItem>> allItems(FrameIdentifier frame) override {
        Vector<Ref<HistoryItem>> result;
        for (auto &entry : m_items)
            if (auto *item = findFrame(entry.item, frame))
                result.append(Ref{*item});
        return result;
    }

    RefPtr<HistoryItem> itemAtIndex(int offset, FrameIdentifier frame) override {
        int index = static_cast<int>(m_cursor) + offset;
        if (index < 0 || static_cast<size_t>(index) >= m_items.size())
            return nullptr;
        return findFrame(m_items[index].item, frame);
    }

    unsigned backListCount() const override {
        return m_items.empty() ? 0 : m_cursor;
    }

    unsigned forwardListCount() const override {
        return m_items.empty() ? 0 : m_items.size() - m_cursor - 1;
    }

    bool containsItem(const HistoryItem &item) const override {
        for (auto &entry : m_items)
            if (entry.item->itemID() == item.itemID())
                return true;
        return false;
    }

    void close() override {
        m_items.clear();
        m_cursor = 0;
    }

  private:
    std::vector<Entry> m_items;
    size_t m_cursor = 0;
    int m_next = 1;
    bool m_traversal = false;
    Changed m_changed;

    static HistoryItem *findFrame(HistoryItem &item, FrameIdentifier frame) {
        if (item.frameID() == frame)
            return &item;
        for (auto &child : item.children())
            if (auto *found = findFrame(child, frame))
                return found;
        return nullptr;
    }

    static HistoryItem *findFrameItem(HistoryItem &item, BackForwardFrameItemIdentifier frame) {
        if (item.frameItemID() == frame)
            return &item;
        for (auto &child : item.children())
            if (auto *found = findFrameItem(child, frame))
                return found;
        return nullptr;
    }
};
}
