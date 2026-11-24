#include "Ui.h"

namespace ui {

using namespace colours;

juce::Font font (float size, bool bold)
{
    return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
}

namespace {
const int kAttackMs[16] = { 2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000 };
const int kDecayMs[16] = { 6, 24, 48, 72, 114, 168, 204, 240, 300, 750, 1500, 2400, 3000, 9000, 15000, 24000 };

juce::String ms (int v) { return v < 1000 ? juce::String (v) + " ms" : juce::String (v / 1000.0, v % 1000 ? 1 : 0) + " s"; }
} // namespace

juce::String attackTime (int n) { return ms (kAttackMs[juce::jlimit (0, 15, n)]); }
juce::String decayReleaseTime (int n) { return ms (kDecayMs[juce::jlimit (0, 15, n)]); }

// ---------------------------------------------------------------- look and feel

LookAndFeel::LookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, bg);
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::TextButton::buttonColourId, field);
    setColour (juce::TextButton::buttonOnColourId, selBg);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, bright);
    setColour (juce::ComboBox::backgroundColourId, field);
    setColour (juce::ComboBox::textColourId, bright);
    setColour (juce::ComboBox::outlineColourId, border);
    setColour (juce::ComboBox::arrowColourId, label);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff2a2e47));
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, selBg);
    setColour (juce::PopupMenu::highlightedTextColourId, bright);
    setColour (juce::TextEditor::backgroundColourId, field);
    setColour (juce::TextEditor::textColourId, bright);
    setColour (juce::TextEditor::outlineColourId, border);
    setColour (juce::TextEditor::focusedOutlineColourId, selBorder);
    setColour (juce::TextEditor::highlightColourId, selBg);
    setColour (juce::CaretComponent::caretColourId, bright);
    setColour (juce::Label::textColourId, dim);
    setColour (juce::ToggleButton::textColourId, juce::Colour (0xffc3c8e3));
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff2a2e47));
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, border);
}

void LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s)
{
    auto box = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    auto r = juce::jmin (box.getWidth(), box.getHeight()) / 2.0f;
    auto c = box.getCentre();
    float thick = juce::jmax (4.0f, r * 0.2f);
    juce::Path track, value;
    track.addCentredArc (c.x, c.y, r - thick / 2, r - thick / 2, 0.0f, start, end, true);
    g.setColour (border);
    g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (pos > 0.001f)
    {
        value.addCentredArc (c.x, c.y, r - thick / 2, r - thick / 2, 0.0f, start, start + pos * (end - start), true);
        g.setColour (s.findColour (juce::Slider::rotarySliderFillColourId));
        g.strokePath (value, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    if (s.hasKeyboardFocus (true))
    {
        g.setColour (selBorder.withAlpha (0.6f));
        g.drawEllipse (box.reduced (-2.0f), 1.0f);
    }
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                    juce::Slider::SliderStyle, juce::Slider&)
{
    auto bar = juce::Rectangle<float> ((float) x, (float) y + (float) h / 2.0f - 3.0f, (float) w, 6.0f);
    g.setColour (border);
    g.fillRoundedRectangle (bar, 3.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (bar.withRight (pos), 3.0f);
    g.setColour (bright);
    g.fillEllipse (pos - 7.0f, bar.getCentreY() - 7.0f, 14.0f, 14.0f);
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& base, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto fill = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : base;
    if (down) fill = fill.brighter (0.15f);
    else if (over) fill = fill.brighter (0.07f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 8.0f);
    auto edge = b.getProperties().getWithDefault ("border", (int) border.getARGB());
    g.setColour (juce::Colour ((juce::uint32) (int) edge));
    g.drawRoundedRectangle (r, 8.0f, 1.0f);
}

void LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId));
    int padLeft = b.getProperties().getWithDefault ("padLeft", 0);   // room for an icon / dot
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (8, 2).withTrimmedLeft (padLeft),
                      padLeft > 0 ? juce::Justification::centredLeft : juce::Justification::centred, 1);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    // a switch, then the label
    auto r = b.getLocalBounds();
    auto sw = r.removeFromLeft (34).withSizeKeepingCentre (34, 20).toFloat();
    bool isOn = b.getToggleState();
    g.setColour (isOn ? juce::Colour (0xff5b67c9) : border.brighter (over ? 0.1f : 0.0f));
    g.fillRoundedRectangle (sw, 10.0f);
    g.setColour (isOn ? bright : dim);
    g.fillEllipse (isOn ? sw.getRight() - 18.0f : sw.getX() + 2.0f, sw.getY() + 2.0f, 16.0f, 16.0f);
    r.removeFromLeft (8);
    g.setColour (b.findColour (juce::ToggleButton::textColourId));
    g.setFont (font (12.5f));
    g.drawFittedText (b.getButtonText(), r, juce::Justification::centredLeft, 2);
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (box.hasKeyboardFocus (true) ? selBorder : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 8.0f, 1.0f);
    juce::Path chevron;
    float cx = (float) w - 16.0f, cy = (float) h / 2.0f;
    chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
    chevron.lineTo (cx, cy + 2.0f);
    chevron.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (label);
    g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void LookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& l)
{
    l.setBounds (10, 1, box.getWidth() - 34, box.getHeight() - 2);
    l.setFont (getComboBoxFont (box));
}

void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& t)
{
    g.setColour (t.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h), 8.0f);
}

void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& t)
{
    g.setColour (t.hasKeyboardFocus (true) ? selBorder : border);
    g.drawRoundedRectangle (juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f), 8.0f, 1.0f);
}

// ---------------------------------------------------------------- chip / segmented

void Chip::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    auto fill = selected ? selBg : field;
    if (down) fill = fill.brighter (0.15f);
    else if (over) fill = fill.brighter (0.07f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 7.0f);
    g.setColour (selected ? selBorder : border);
    g.drawRoundedRectangle (r, 7.0f, 1.0f);
    g.setColour (selected ? bright : dim);
    auto area = getLocalBounds();
    if (! icon.isEmpty())
    {
        auto iconBox = area.removeFromTop (getHeight() / 2 + 2).withTrimmedTop (5).toFloat();
        auto p = icon;
        p.applyTransform (p.getTransformToScaleToFit (iconBox.withSizeKeepingCentre (26.0f, 11.0f), true));
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::mitered));
    }
    g.setFont (font (12.0f, true));
    g.drawFittedText (getButtonText(), area.reduced (4, 0), juce::Justification::centred, 1);
}

Segmented::Segmented (juce::StringArray labels)
{
    for (int i = 0; i < labels.size(); ++i)
    {
        auto c = std::make_unique<Chip> (labels[i]);
        c->onClick = [this, i] {
            setSelected (i);
            if (onChange)
                onChange (i);
        };
        addAndMakeVisible (*c);
        chips.push_back (std::move (c));
    }
}

void Segmented::setSelected (int index)
{
    current = index;
    for (size_t i = 0; i < chips.size(); ++i)
        chips[i]->setSelected ((int) i == index);
}

void Segmented::paint (juce::Graphics& g)
{
    g.setColour (field);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
}

void Segmented::resized()
{
    auto r = getLocalBounds().reduced (2);
    int w = r.getWidth() / juce::jmax (1, (int) chips.size());
    for (size_t i = 0; i < chips.size(); ++i)
        chips[i]->setBounds (i + 1 == chips.size() ? r : r.removeFromLeft (w));
}

// ---------------------------------------------------------------- knob

Knob::Knob (const juce::String& l, std::function<juce::String (double)> format, int d)
    : label (l), fmt (std::move (format)), diameter (d)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    s.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    s.onValueChange = [this] { repaint(); };
    s.onRightClick = [this] { if (onRightClick) onRightClick(); };
    s.setTitle (l);
    addAndMakeVisible (s);
}

void Knob::setBadge (const juce::String& newBadge, bool isLearning)
{
    if (newBadge == badge && isLearning == learning)
        return;
    badge = newBadge;
    learning = isLearning;
    s.setColour (juce::Slider::rotarySliderFillColourId, learning ? warn : accent);
    repaint();
}

void Knob::resized()
{
    auto r = getLocalBounds();
    s.setBounds (r.removeFromTop (diameter).withSizeKeepingCentre (diameter, diameter));
}

void Knob::paint (juce::Graphics& g)
{
    if (learning)
    {
        g.setColour (warn.withAlpha (0.08f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 10.0f);
        g.setColour (warn);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 10.0f, 2.0f);
    }
    auto r = getLocalBounds().withTrimmedTop (diameter);
    g.setColour (bright);
    g.setFont (font (14.0f, true));
    g.drawText (fmt ? fmt (s.getValue()) : juce::String (s.getValue()), r.removeFromTop (18), juce::Justification::centred);
    g.setColour (dim);
    g.setFont (font (11.5f));
    g.drawText (label, r.removeFromTop (15), juce::Justification::centred);
    if (hint.isNotEmpty())
    {
        g.setColour (colours::label);
        g.setFont (font (10.5f));
        g.drawText (hint, r.removeFromTop (14), juce::Justification::centred);
    }
    auto shown = learning ? juce::String ("Move a control...") : badge;
    if (shown.isNotEmpty())
    {
        auto f = font (10.0f, true);
        int w = juce::GlyphArrangement::getStringWidthInt (f, shown) + 12;
        auto b = r.removeFromTop (18).withSizeKeepingCentre (juce::jmin (w, getWidth()), 16).toFloat();
        g.setColour (learning ? warn : juce::Colour (0xff2f3557));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (learning ? juce::Colour (0xff2a1d07) : juce::Colour (0xffc3c8e3));
        g.setFont (f);
        g.drawText (shown, b, juce::Justification::centred);
    }
}

// ---------------------------------------------------------------- envelope / voices / card

void EnvelopeGraph::setAdsr (int a, int d, int s, int r)
{
    if (a == attack && d == decay && s == sustain && r == release)
        return;
    attack = a; decay = d; sustain = s; release = r;
    repaint();
}

void EnvelopeGraph::paint (juce::Graphics& g)
{
    auto box = getLocalBounds().toFloat();
    g.setColour (field);
    g.fillRoundedRectangle (box, 8.0f);
    auto r = box.reduced (8.0f, 8.0f);
    // segment widths: log-ish of the real times, so short and long settings both read
    auto seg = [] (int ms) { return std::log10 (1.0f + (float) ms) ; };
    float a = seg (kAttackMs[attack]), d = seg (kDecayMs[decay]), hold = 3.0f, rl = seg (kDecayMs[release]);
    float scale = r.getWidth() / (a + d + hold + rl);
    float x0 = r.getX(), base = r.getBottom(), top = r.getY();
    float sy = base - (base - top) * (float) sustain / 15.0f;
    juce::Path p;
    p.startNewSubPath (x0, base);
    p.lineTo (x0 + a * scale, top);
    p.lineTo (x0 + (a + d) * scale, sy);
    p.lineTo (x0 + (a + d + hold) * scale, sy);
    p.lineTo (x0 + (a + d + hold + rl) * scale, base);
    auto fill = p;
    fill.closeSubPath();
    g.setColour (accent.withAlpha (0.16f));
    g.fillPath (fill);
    g.setColour (accent);
    g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::mitered));
}

void VoiceStrip::setVoices (std::vector<Voice> v)
{
    bool same = v.size() == voices.size();
    for (size_t i = 0; same && i < v.size(); ++i)
        same = v[i].where == voices[i].where && v[i].note == voices[i].note && v[i].on == voices[i].on;
    if (! same)
    {
        voices = std::move (v);
        repaint();
    }
}

void VoiceStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    g.setColour (colours::label);
    g.setFont (font (11.0f, true));
    g.drawText ("VOICES", r.removeFromLeft (64), juce::Justification::centredLeft);
    if (voices.empty())
        return;
    int w = r.getWidth() / (int) voices.size();
    for (auto& v : voices)
    {
        auto cell = r.removeFromLeft (w).reduced (3, 0).toFloat();
        g.setColour (v.on ? on : panel);
        g.fillRoundedRectangle (cell, 8.0f);
        g.setColour (v.on ? onText : colours::label);
        g.setFont (font (10.0f));
        g.drawText (v.where, cell.withTrimmedBottom (cell.getHeight() / 2).withTrimmedTop (4), juce::Justification::centred);
        g.setFont (font (14.0f, true));
        g.drawText (v.note, cell.withTrimmedTop (cell.getHeight() / 2 - 4), juce::Justification::centredTop);
    }
}

void Card::paint (juce::Graphics& g)
{
    g.setColour (panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 12.0f);
    auto r = getLocalBounds().reduced (14);
    if (step > 0)
    {
        auto head = r.removeFromTop (22);
        auto dot = head.removeFromLeft (22).toFloat();
        g.setColour (stepDone ? on : border);
        g.fillEllipse (dot);
        g.setColour (stepDone ? onText : text);
        g.setFont (font (12.0f, true));
        g.drawText (stepDone ? juce::String (juce::CharPointer_UTF8 ("\xe2\x9c\x93")) : juce::String (step), dot, juce::Justification::centred);
        head.removeFromLeft (10);
        g.setColour (bright);
        g.setFont (font (13.5f, true));
        g.drawText (title, head, juce::Justification::centredLeft);
        if (stepNote.isNotEmpty())
        {
            int tw = juce::GlyphArrangement::getStringWidthInt (font (13.5f, true), title);
            g.setColour (colours::label);
            g.setFont (font (12.5f));
            g.drawText (stepNote, head.withTrimmedLeft (tw + 6), juce::Justification::centredLeft);
        }
    }
    else
    {
        g.setColour (colours::label);
        g.setFont (font (11.0f, true));
        g.drawText (title.toUpperCase(), r.removeFromTop (14), juce::Justification::centredLeft);
    }
}

} // namespace ui
