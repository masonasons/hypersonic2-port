#include "../Source/KeyboardChoice.h"
#include "../Source/KeyboardNumber.h"
#include "../Source/KeyboardShortcut.h"
#include "../Source/MusicalTyping.h"
#include "../Source/ReviewText.h"

#include <iostream>
#include <tuple>
#include <vector>

static int checks() {
    using Key = juce::KeyPress;
    const auto alt = juce::ModifierKeys::altModifier;
    if (hs2port::mnemonicLetter(Key('S', alt, 0)) != 's'
        || hs2port::mnemonicLetter(Key(Key::F4Key, alt, 0)) != 0
        || hs2port::mnemonicLetter(Key(Key::F1Key, alt, 0)) != 0) return 15;
    using hs2port::keyboardNumber;
    if (keyboardNumber("+1.25") != 1.25 || keyboardNumber("-.5") != -.5
        || keyboardNumber("1e2") != 100.0 || keyboardNumber("") || keyboardNumber("+-1")
        || keyboardNumber("1,25") || keyboardNumber("2 dB") || keyboardNumber("nan")
        || keyboardNumber("1e999")) return 14;
    hs2port::ReviewText info;
    info.setSize(180, 48);
    juce::String document;
    for (int i = 1; i <= 20; ++i) document += "Patch detail " + juce::String(i) + "\n";
    info.setText(document, false);
    info.setCaretPosition(0);
    if (!info.keyPressed(Key(Key::downKey)) || info.getCaretPosition() <= 0) return 1;
    const auto line = info.getCaretPosition();
    if (!info.keyPressed(Key(Key::pageDownKey)) || info.getCaretPosition() <= line) return 2;
    if (!info.keyPressed(Key(Key::homeKey)) || info.getCaretPosition() != 0 || info.getText() != document) return 3;
    const auto accessible = info.createAccessibilityHandler();
    const auto* text = accessible->getTextInterface();
    if (text == nullptr || !text->isReadOnly()) return 4;
    info.setCaretPosition(line);
    info.refreshText(document);
    if (info.getCaretPosition() != line) return 11;
    info.refreshText(document + "New status line\n");
    if (info.getCaretPosition() != line || !info.keyPressed(Key(Key::downKey))
        || info.getCaretPosition() <= line) return 12;
    info.refreshText("OK");
    if (info.getCaretPosition() != 2) return 13;

    hs2port::KeyboardChoice choice;
    for (int i = 1; i <= 16; ++i) choice.addItem("Part " + juce::String(i), i * 2);
    choice.setSelectedItemIndex(0);
    if (!choice.keyPressed(Key(Key::pageDownKey)) || choice.getSelectedId() != 22) return 5;
    if (!choice.keyPressed(Key(Key::pageDownKey)) || choice.getSelectedId() != 32) return 6;
    if (!choice.keyPressed(Key(Key::upKey)) || choice.getSelectedId() != 30) return 7;
    if (!choice.keyPressed(Key(Key::homeKey)) || choice.getSelectedId() != 2) return 8;

    hs2port::MusicalTyping keyboard;
    std::vector<std::tuple<int, int, int>> events;
    auto send = [&](int channel, int note, int velocity) { events.emplace_back(channel, note, velocity); };
    const auto upperF = keyboard.keyIndex('\'');
    if (upperF < 0) return 9;
    keyboard.start(false);
    keyboard.press(upperF, upperF, 7, send);
    keyboard.shiftOctave(1);
    keyboard.releaseAll(send);
    if (events != std::vector<std::tuple<int, int, int>>{{7, 77, 88}, {7, 77, 0}}) return 10;
    std::cout << "PASS: review text caret, paging and refresh; selector steps; musical typing and note release" << std::endl;
    return 0;
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    const auto result = checks();
    std::cout << "Keyboard checks result " << result << std::endl;
    return result;
}
