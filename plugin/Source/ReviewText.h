#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace hs2port {
// Caret navigation and text accessibility follow Sound Canvas Bridge (OnjMusic, MIT).
class ReviewText final : public juce::TextEditor {
    class TextAccess final : public juce::AccessibilityTextInterface {
    public:
        explicit TextAccess(ReviewText& view) : editor(view) {}
        bool isDisplayingProtectedText() const override { return false; }
        bool isReadOnly() const override { return true; }
        int getTotalNumCharacters() const override { return editor.getText().length(); }
        juce::Range<int> getSelection() const override { return editor.getHighlightedRegion(); }
        void setSelection(juce::Range<int> range) override { editor.setHighlightedRegion(range); }
        juce::String getText(juce::Range<int> range) const override { return editor.getTextInRange(range); }
        void setText(const juce::String&) override {}
        int getTextInsertionOffset() const override { return editor.getCaretPosition(); }
        juce::RectangleList<int> getTextBounds(juce::Range<int> range) const override {
            juce::RectangleList<int> result;
            for (const auto& rect : editor.getTextBounds(range)) result.add(editor.localAreaToGlobal(rect));
            return result;
        }
        int getOffsetAtPoint(juce::Point<int> point) const override {
            return editor.getTextIndexAt(editor.getLocalPoint(nullptr, point));
        }
    private:
        ReviewText& editor;
    };
public:
    ReviewText() {
        setMultiLine(true, true);
        setReadOnly(true);
        setCaretVisible(true);
        setTabKeyUsedAsCharacter(false);
    }
    void refreshText(const juce::String& text) {
        if (getText() == text) return;
        const auto caret = getCaretPosition();
        const auto selection = getHighlightedRegion();
        setText(text, false);
        const auto length = getTotalNumChars();
        setCaretPosition(juce::jmin(caret, length));
        if (!selection.isEmpty())
            setHighlightedRegion({juce::jmin(selection.getStart(), length), juce::jmin(selection.getEnd(), length)});
    }
    bool keyPressed(const juce::KeyPress& key) override {
        const auto mods = key.getModifiers();
        const auto selecting = mods.isShiftDown();
        if (mods.isCtrlDown()) return TextEditor::keyPressed(key);
        const auto code = key.getKeyCode();
        if (code == juce::KeyPress::upKey) return mods.isCommandDown() ? moveCaretToTop(selecting) : moveCaretUp(selecting);
        if (code == juce::KeyPress::downKey) return mods.isCommandDown() ? moveCaretToEnd(selecting) : moveCaretDown(selecting);
        if (code == juce::KeyPress::leftKey) return mods.isCommandDown() ? moveCaretToStartOfLine(selecting) : moveCaretLeft(mods.isAltDown(), selecting);
        if (code == juce::KeyPress::rightKey) return mods.isCommandDown() ? moveCaretToEndOfLine(selecting) : moveCaretRight(mods.isAltDown(), selecting);
        if (code == juce::KeyPress::homeKey) return moveCaretToTop(selecting);
        if (code == juce::KeyPress::endKey) return moveCaretToEnd(selecting);
        if (code == juce::KeyPress::pageUpKey) return pageUp(selecting);
        if (code == juce::KeyPress::pageDownKey) return pageDown(selecting);
        return TextEditor::keyPressed(key);
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
        return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::editableText,
            juce::AccessibilityActions{}, juce::AccessibilityHandler::Interfaces{std::make_unique<TextAccess>(*this)});
    }
};
}
