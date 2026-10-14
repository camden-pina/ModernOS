// Real local editing policies, upstream WPE keyboard commands, and bounded
// UndoStep ownership. Clipboard, IME and spelling services are unavailable.
#pragma once

#include "DOMPasteAccess.h"
#include "EditorClient.h"
#include "UndoStep.h"
#include <wtf/TZoneMallocInlines.h>

namespace ModernOS {
using namespace WebCore;
void handleModernOSKeyboardEvent(KeyboardEvent &);
class EditorModernOS final : public EditorClient {
    WTF_MAKE_TZONE_ALLOCATED(EditorModernOS);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(EditorModernOS);

  public:
    ~EditorModernOS() override {
        clearUndoRedoOperations();
    }

  private:
    bool shouldDeleteRange(const std::optional<SimpleRange> &) override {
        return true;
    }

    bool smartInsertDeleteEnabled() override {
        return false;
    }

    bool isSelectTrailingWhitespaceEnabled() const override {
        return false;
    }

    bool isContinuousSpellCheckingEnabled() override {
        return false;
    }

    void toggleContinuousSpellChecking() override {
    }

    bool isGrammarCheckingEnabled() override {
        return false;
    }

    void toggleGrammarChecking() override {
    }

    int spellCheckerDocumentTag() override {
        return -1;
    }

    bool shouldBeginEditing(const SimpleRange &) override {
        return true;
    }

    bool shouldEndEditing(const SimpleRange &) override {
        return true;
    }

    bool shouldInsertNode(Node &, const std::optional<SimpleRange> &, EditorInsertAction) override {
        return true;
    }

    bool shouldInsertText(const String &, const std::optional<SimpleRange> &,
                          EditorInsertAction) override {
        return true;
    }

    bool shouldChangeSelectedRange(const std::optional<SimpleRange> &,
                                   const std::optional<SimpleRange> &, Affinity, bool) override {
        return true;
    }

    bool shouldApplyStyle(const StyleProperties &, const std::optional<SimpleRange> &) override {
        return true;
    }

    bool shouldMoveRangeAfterDelete(const SimpleRange &, const SimpleRange &) override {
        return true;
    }

    void didApplyStyle() override {
    }

    void didBeginEditing() override {
    }

    void didEndEditing() override {
    }

    void respondToChangedContents() override {
    }

    void respondToChangedSelection(LocalFrame *) override {
    }

    void updateEditorStateAfterLayoutIfEditabilityChanged() override {
    }

    void didEndUserTriggeredSelectionChanges() override {
    }

    void discardedComposition(const Document &) override {
    }

    void canceledComposition() override {
    }

    void didUpdateComposition() override {
    }

    void willWriteSelectionToPasteboard(const std::optional<SimpleRange> &) override {
    }

    void didWriteSelectionToPasteboard() override {
    }

    void getClientPasteboardData(const std::optional<SimpleRange> &,
                                 Vector<std::pair<String, RefPtr<SharedBuffer>>> &) override {
    }

    DOMPasteAccessResponse requestDOMPasteAccess(DOMPasteAccessCategory, FrameIdentifier,
                                                 const String &) override {
        return DOMPasteAccessResponse::DeniedForGesture;
    }

    bool canCopyCut(LocalFrame *, bool) const override {
        return false;
    }

    bool canPaste(LocalFrame *, bool) const override {
        return false;
    }

    void registerUndoStep(UndoStep &step) override {
        if (!m_redoing)
            clear(m_redo);
        append(m_undo, step);
    }

    void registerRedoStep(UndoStep &step) override {
        append(m_redo, step);
    }

    void clearUndoRedoOperations() override {
        clear(m_undo);
        clear(m_redo);
    }

    bool canUndo() const override {
        return !m_undo.isEmpty();
    }

    bool canRedo() const override {
        return !m_redo.isEmpty();
    }

    void undo() override {
        if (!m_undo.isEmpty()) {
            auto step = m_undo.takeLast();
            step->unapply();
        }
    }

    void redo() override {
        if (!m_redo.isEmpty()) {
            auto step = m_redo.takeLast();
            m_redoing = true;
            step->reapply();
            m_redoing = false;
        }
    }

    void handleKeyboardEvent(KeyboardEvent &event) override {
        handleModernOSKeyboardEvent(event);
    }

    void handleInputMethodKeydown(KeyboardEvent &) override {
    }

    void textFieldDidBeginEditing(Element &) override {
    }

    void textFieldDidEndEditing(Element &) override {
    }

    void textDidChangeInTextField(Element &) override {
    }

    bool doTextFieldCommandFromEvent(Element &, KeyboardEvent *) override {
        return false;
    }

    void textWillBeDeletedInTextField(Element &) override {
    }

    void textDidChangeInTextArea(Element &) override {
    }

    void overflowScrollPositionChanged() override {
    }

    void subFrameScrollPositionChanged() override {
    }

    TextCheckerClient *textChecker() override {
        return nullptr;
    }

    void updateSpellingUIWithGrammarString(const String &, const GrammarDetail &) override {
    }

    void updateSpellingUIWithMisspelledWord(const String &) override {
    }

    void showSpellingUI(bool) override {
    }

    bool spellingUIIsShowing() override {
        return false;
    }

    void setInputMethodState(Element *) override {
    }

    bool performTwoStepDrop(DocumentFragment &, const SimpleRange &, bool) override {
        return false;
    }

    Vector<Ref<UndoStep>> m_undo, m_redo;
    bool m_redoing = false;

    static void clear(Vector<Ref<UndoStep>> &steps) {
        for (auto &step : steps)
            step->didRemoveFromUndoManager();
        steps.clear();
    }

    static void append(Vector<Ref<UndoStep>> &steps, UndoStep &step) {
        if (!steps.isEmpty() && steps.last().ptr() == &step)
            return;
        if (steps.size() == 64) {
            steps[0]->didRemoveFromUndoManager();
            steps.removeAt(0);
        }
        steps.append(Ref{step});
    }
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(EditorModernOS);
}
