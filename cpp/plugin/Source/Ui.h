// SID Bridge's look and its small widgets: knobs, toggle chips, segmented controls, the envelope
// drawing, the voice strip, card backgrounds.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace ui {

namespace colours {
const juce::Colour bg (0xff1b1d2a), panel (0xff232639), field (0xff1d2033), border (0xff343957),
    text (0xffd8dcf0), bright (0xffffffff), dim (0xffa3a9c7), label (0xff8a91b4), accent (0xff8f9bff),
    selBg (0xff3a4170), selBorder (0xff6f7cf0), on (0xff6fdc8c), onText (0xff13261a), warn (0xffffb454),
    warnBg (0xff3a2f1c), err (0xffff6b6b), silenceBg (0xff3a2229), silenceBorder (0xff6b3a44),
    silenceText (0xffffb3bd), readyBg (0xff1f3328), readyBorder (0xff2f6b45), readyText (0xffa9d9b8);
}

juce::Font font (float size, bool bold = false);

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (13.0f); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return font (12.5f, true); }
    juce::Font getPopupMenuFont() override { return font (13.0f); }
    juce::Font getLabelFont (juce::Label&) override { return font (12.0f); }
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
};

/// Right-click (or ctrl-click) calls onRightClick instead of acting as a normal click.
template <class Base>
struct RightClickable : Base
{
    using Base::Base;
    std::function<void()> onRightClick;
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && onRightClick)
            onRightClick();
        else
            Base::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (! (e.mods.isPopupMenu() && onRightClick)) Base::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override { if (! (e.mods.isPopupMenu() && onRightClick)) Base::mouseUp (e); }
};

/// A text button that shows a selected state (setSelected), optionally with a small icon path.
class Chip : public RightClickable<juce::TextButton>
{
public:
    explicit Chip (const juce::String& text = {}) { setButtonText (text); }
    void setSelected (bool s) { if (s != selected) { selected = s; repaint(); } }
    bool isSelected() const { return selected; }
    juce::Path icon;   // drawn above the text, in a 28x12 box
    void paintButton (juce::Graphics&, bool over, bool down) override;
private:
    bool selected = false;
};

/// A row of chips where exactly one is selected.
class Segmented : public juce::Component
{
public:
    explicit Segmented (juce::StringArray labels);
    void setSelected (int index);
    int selected() const { return current; }
    std::function<void (int)> onChange;
    void resized() override;
    void paint (juce::Graphics&) override;
    Chip& chip (int i) { return *chips[(size_t) i]; }
    void setTooltip (const juce::String& t) { for (auto& c : chips) c->setTooltip (t); }
private:
    std::vector<std::unique_ptr<Chip>> chips;
    int current = -1;
};

/// A rotary knob with its value, label, a hint line and an optional badge (what controls it).
class Knob : public juce::Component
{
public:
    Knob (const juce::String& label, std::function<juce::String (double)> format, int diameter = 64);
    juce::Slider& slider() { return s; }
    void setHint (const juce::String& h) { if (h != hint) { hint = h; repaint(); } }
    void setBadge (const juce::String& text, bool learning);
    std::function<void()> onRightClick;
    void paint (juce::Graphics&) override;
    void resized() override;
    const juce::String& badgeText() const { return badge; }
private:
    struct S : RightClickable<juce::Slider> {};
    S s;
    juce::String label, hint, badge;
    bool learning = false;
    std::function<juce::String (double)> fmt;
    int diameter;
};

/// The ADSR shape.
class EnvelopeGraph : public juce::Component
{
public:
    void setAdsr (int a, int d, int s, int r);
    void paint (juce::Graphics&) override;
private:
    int attack = 0, decay = 9, sustain = 10, release = 4;
};

struct Voice
{
    juce::String where, note;
    bool on = false;
};

class VoiceStrip : public juce::Component
{
public:
    void setVoices (std::vector<Voice> v);
    void paint (juce::Graphics&) override;
private:
    std::vector<Voice> voices;
};

/// A rounded card with a small caps title (or a numbered step heading).
class Card : public juce::Component
{
public:
    explicit Card (const juce::String& t) : title (t) { setInterceptsMouseClicks (false, true); }
    void setStep (int number, bool done, const juce::String& note = {}) { step = number; stepDone = done; stepNote = note; repaint(); }
    void paint (juce::Graphics&) override;
    /// The area for the card's controls, in the parent's coordinates (they are the parent's children).
    juce::Rectangle<int> content() const { return getBounds().reduced (14).withTrimmedTop (step > 0 ? 34 : 24); }
private:
    juce::String title, stepNote;
    int step = 0;
    bool stepDone = false;
};

/// SID envelope times by nibble (datasheet), for the knob hints and the drawing.
juce::String attackTime (int nibble);
juce::String decayReleaseTime (int nibble);

} // namespace ui
