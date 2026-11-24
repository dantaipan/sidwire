#include "PlayPage.h"

using namespace msid;
using namespace ui::colours;

namespace {
juce::Path wavePath (int i)
{
    juce::Path p;
    switch (i)
    {
        case 0: p.startNewSubPath (1, 11); p.lineTo (7.5f, 1); p.lineTo (14, 11); p.lineTo (20.5f, 1); p.lineTo (27, 11); break;
        case 1: p.startNewSubPath (1, 11); p.lineTo (13, 1); p.lineTo (13, 11); p.lineTo (25, 1); p.lineTo (25, 11); break;
        case 2: p.startNewSubPath (1, 11); p.lineTo (1, 1); p.lineTo (9, 1); p.lineTo (9, 11); p.lineTo (17, 11);
                p.lineTo (17, 1); p.lineTo (25, 1); p.lineTo (25, 11); break;
        default:
        {
            const float ys[] = { 7, 2, 10, 4, 9, 1, 8, 3, 11, 5, 9, 2, 8, 5 };
            p.startNewSubPath (1, ys[0]);
            for (int k = 1; k < 14; ++k)
                p.lineTo (1.0f + (float) k * 2.0f, ys[k]);
        }
    }
    return p;
}

const char* kWaveNames[] = { "tri", "saw", "pulse", "noise" };
const juce::String kDot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
const char* kModeNames[] = { "lp", "bp", "hp" };
} // namespace

PlayPage::PlayPage (SidBridgeProcessor& p) : proc (p)
{
    prevPreset.setButtonText (juce::CharPointer_UTF8 ("\xe2\x80\xb9"));
    nextPreset.setButtonText (juce::CharPointer_UTF8 ("\xe2\x80\xba"));
    prevPreset.setTooltip ("Previous preset (right-click: MIDI learn)");
    nextPreset.setTooltip ("Next preset (right-click: MIDI learn)");
    auto step = [this] (int d) {
        auto list = proc.bridge().presets();
        if (list.empty())
            return;
        int cur = preset.getSelectedId() - 1;
        int n = (int) list.size();
        int next = cur < 0 ? (d > 0 ? 0 : n - 1) : ((cur + d) % n + n) % n;
        proc.bridge().selectPreset (next);
    };
    prevPreset.onClick = [step] { step (-1); };
    nextPreset.onClick = [step] { step (1); };
    prevPreset.onRightClick = [this] { learnMenu ("action:preset_prev", prevPreset); };
    nextPreset.onRightClick = [this] { learnMenu ("action:preset_next", nextPreset); };
    preset.setComponentID ("preset");
    preset.setTextWhenNothingSelected ("init");
    preset.onChange = [this] {
        auto id = preset.getSelectedId();
        if (id == kCmdLoadPresets)
            choosePresetsFile();
        else if (id == kCmdBuiltinPresets)
        {
            auto s = proc.bridge().getSettings();
            s.presetsFile = {};
            proc.bridge().setSettings (s);
        }
        else if (id > 0)
            proc.bridge().selectPreset (id - 1);
        if (id >= kCmdLoadPresets)
            shownPresetsVersion = -1;
    };
    info.setJustificationType (juce::Justification::centredRight);
    info.setColour (juce::Label::textColourId, label);
    bannerButton.setVisible (false);
    for (auto* c : { &prevPreset, &nextPreset, &bannerButton })
        addAndMakeVisible (*c);
    addAndMakeVisible (preset);
    addAndMakeVisible (info);

    for (auto* c : { &osc, &filter, &env, &out })
        addAndMakeVisible (*c);

    for (int i = 0; i < 4; ++i)
    {
        auto& w = waves[(size_t) i];
        w.setButtonText (juce::String (kWaveNames[i]).substring (0, 1).toUpperCase() + juce::String (kWaveNames[i]).substring (1));
        w.icon = wavePath (i);
        w.setTooltip ("Light two to combine them, as the SID does (right-click: MIDI learn \"next waveform\")");
        w.onClick = [this, i] { clickWave (i); };
        w.onRightClick = [this, &w] { learnMenu ("action:waveform_next", w); };
        addAndMakeVisible (w);
    }
    for (int i = 0; i < 3; ++i)
    {
        auto& m = modes[(size_t) i];
        m.setButtonText (juce::String (kModeNames[i]).toUpperCase());
        m.setTooltip ("Low / band / high pass. None lit = filter off; LP + HP = notch (right-click: MIDI learn \"next filter mode\")");
        m.onClick = [this, i] { clickMode (i); };
        m.onRightClick = [this, &m] { learnMenu ("action:filter_mode_next", m); };
        addAndMakeVisible (m);
    }
    for (auto* l : { &waveHint, &modeHint, &velocityLabel, &amountLabel })
    {
        l->setColour (juce::Label::textColourId, label);
        l->setFont (ui::font (11.0f));
        addAndMakeVisible (*l);
    }
    waveHint.setText ("Light two to combine them.", juce::dontSendNotification);
    modeHint.setText ("None lit = off. LP + HP = notch.", juce::dontSendNotification);
    velocityLabel.setText ("Velocity", juce::dontSendNotification);
    velocityLabel.setColour (juce::Label::textColourId, dim);
    amountLabel.setColour (juce::Label::textColourId, dim);

    const char* labels[kNumParams] = { "Pulse width", "Cutoff", "Resonance", "Attack", "Decay", "Sustain", "Release", "Volume" };
    for (int i = 0; i < kNumParams; ++i)
    {
        auto prm = (Param) i;
        std::function<juce::String (double)> fmt = [] (double v) { return juce::String (juce::roundToInt (v)); };
        if (prm == Param::PulseWidth)
            fmt = [] (double v) { return juce::String (juce::roundToInt (v / 4095.0 * 100.0)) + "%"; };
        int diameter = (prm == Param::Attack || prm == Param::Decay || prm == Param::Sustain || prm == Param::Release) ? 60 : 76;
        auto k = std::make_unique<ui::Knob> (labels[i], fmt, diameter);
        k->setComponentID (paramName (prm));
        juce::String target = "param:" + juce::String (paramName (prm));
        auto* kp = k.get();
        k->onRightClick = [this, target, kp] { learnMenu (target, *kp); };
        k->slider().setTooltip (juce::String (labels[i]) + " (right-click: MIDI learn)");
        addAndMakeVisible (*k);
        attachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, paramName (prm), k->slider()));
        knobs[(size_t) i] = std::move (k);
    }
    addAndMakeVisible (graph);
    velocity.onChange = [this] (int i) { setChoice ("velocity", i); };
    velocity.setTooltip ("How hard you play: Sustain = each note's sustain level, Volume = the SID's master volume");
    addAndMakeVisible (velocity);
    amount.setSliderStyle (juce::Slider::LinearHorizontal);
    amount.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    amount.setTooltip ("Velocity amount: 0% = every note at full level, 100% = level follows velocity");
    addAndMakeVisible (amount);
    attachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, "velocity_amount", amount));
    addAndMakeVisible (voices);

    voiceLabel.setFont (ui::font (11.0f, true));
    voiceLabel.setColour (juce::Label::textColourId, label);
    addAndMakeVisible (voiceLabel);
    voiceModes.onChange = [this] (int i) { setChoice ("voice_mode", i); };
    voiceModes.setTooltip ("How notes use the SID voices (right-click: MIDI learn \"next voice mode\")");
    for (int i = 0; i < 7; ++i)
        voiceModes.chip (i).onRightClick = [this, i] { learnMenu ("action:voice_mode_next", voiceModes.chip (i)); };
    addAndMakeVisible (voiceModes);
    voiceHint.setFont (ui::font (12.0f));
    voiceHint.setColour (juce::Label::textColourId, label);
    addAndMakeVisible (voiceHint);
    for (auto& rateName : arpRateNames()) arpRate.addItem (rateName, arpRate.getNumItems() + 1);
    for (auto& patternName : arpPatternNames()) arpPattern.addItem (patternName, arpPattern.getNumItems() + 1);
    arpRate.setTooltip ("Arpeggio rate (steps follow MIDI clock when it arrives, else the tempo)");
    arpPattern.setTooltip ("Arpeggio pattern");
    rateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "arp_rate", arpRate);
    patternAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "arp_pattern", arpPattern);
    arpOctaves.onChange = [this] (int i) {
        if (auto* prm = proc.apvts.getParameter ("arp_octaves"))
            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (i + 1)));
    };
    arpOctaves.setTooltip ("Octaves the arpeggio climbs");
    for (auto* sl : { &arpGate, &arpBpm })
    {
        sl->setSliderStyle (juce::Slider::LinearBar);
        sl->setColour (juce::Slider::textBoxTextColourId, bright);
        sl->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    }
    arpGate.setTooltip ("Arpeggio gate: how long each step sounds");
    arpBpm.setTooltip ("Arpeggio tempo when there is no MIDI clock or host tempo");
    attachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, "arp_gate", arpGate));
    attachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, "arp_bpm", arpBpm));
    arpGate.setTextValueSuffix ("% gate");
    arpBpm.setTextValueSuffix (" BPM");
    tempoLabel.setFont (ui::font (11.5f));
    tempoLabel.setColour (juce::Label::textColourId, label);
    tempoLabel.setJustificationType (juce::Justification::centred);
    for (juce::Component* c : { (juce::Component*) &arpRate, (juce::Component*) &arpPattern, (juce::Component*) &arpOctaves,
                                (juce::Component*) &arpGate, (juce::Component*) &arpBpm, (juce::Component*) &tempoLabel })
        addChildComponent (*c);

    goSetup.onClick = [this] { if (onGoToSetup) onGoToSetup(); };
    goSetup.getProperties().set ("border", (int) selBorder.getARGB());
    goSetup.setColour (juce::TextButton::buttonColourId, selBg);
    addChildComponent (goSetup);
    fillPresets();
}

PlayPage::~PlayPage() = default;

int PlayPage::choice (const char* id) const
{
    auto* prm = proc.apvts.getParameter (id);
    return prm ? juce::roundToInt (prm->convertFrom0to1 (prm->getValue())) : 0;
}

void PlayPage::setChoice (const char* id, int index)
{
    if (auto* prm = proc.apvts.getParameter (id))
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 ((float) index));
        prm->endChangeGesture();
    }
}

void PlayPage::clickWave (int i)
{
    const auto& names = waveformNames();
    juce::String cur (names[(size_t) juce::jlimit (0, (int) names.size() - 1, choice ("waveform"))]);
    juce::String next;
    if (i == 3)
        next = "noise";   // noise doesn't combine
    else
    {
        bool lit[3];
        for (int k = 0; k < 3; ++k)
            lit[k] = cur != "noise" && juce::StringArray::fromTokens (cur, "+", "").contains (kWaveNames[k]);
        lit[i] = ! lit[i];
        juce::StringArray parts;
        for (int k = 0; k < 3; ++k)
            if (lit[k])
                parts.add (kWaveNames[k]);
        if (parts.isEmpty())
            return;   // keep at least one
        next = parts.joinIntoString ("+");
    }
    auto it = std::find (names.begin(), names.end(), next.toStdString());
    if (it != names.end())
        setChoice ("waveform", (int) (it - names.begin()));
}

void PlayPage::clickMode (int i)
{
    const auto& names = filterModeNames();
    juce::String cur (names[(size_t) juce::jlimit (0, (int) names.size() - 1, choice ("filter_mode"))]);
    bool lit[3];
    for (int k = 0; k < 3; ++k)
        lit[k] = juce::StringArray::fromTokens (cur, "+", "").contains (kModeNames[k]);
    lit[i] = ! lit[i];
    juce::StringArray parts;
    for (int k = 0; k < 3; ++k)
        if (lit[k])
            parts.add (kModeNames[k]);
    auto next = parts.isEmpty() ? juce::String ("off") : parts.joinIntoString ("+");
    auto it = std::find (names.begin(), names.end(), next.toStdString());
    if (it != names.end())
        setChoice ("filter_mode", (int) (it - names.begin()));
}

void PlayPage::learnMenu (const juce::String& target, juce::Component& near)
{
    bool learning = false;
    auto badge = badgeFor (target, proc.bridge().status(), learning);
    juce::PopupMenu m;
    m.addSectionHeader (BridgeController::targetName (target));
    m.addItem (1, "MIDI learn...");
    if (badge.isNotEmpty())
        m.addItem (2, "Forget " + badge);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&near),
                     [this, target] (int r) {
                         if (r == 1) proc.bridge().startLearn (target);
                         if (r == 2) proc.bridge().forget (target);
                     });
}

juce::String PlayPage::badgeFor (const juce::String& target, const BridgeStatus& st, bool& learning)
{
    learning = st.learnTarget == target;
    bool isParam = target.startsWith ("param:");
    auto name = target.fromFirstOccurrenceOf (":", false, false).toStdString();
    juce::String first;
    int count = 0;
    // the knob / button on the controls port first: that's usually the one under your hand
    for (auto port : { Port::Controls, Port::Notes })
        for (auto& c : st.profile.controls)
            if (c.port == port && (isParam ? c.param == name : c.action == name) && count++ == 0)
                first = shortTrigger (c);
    return count > 1 ? first + " +" + juce::String (count - 1) : first;
}

void PlayPage::fillPresets()
{
    auto presets = proc.bridge().presets();
    preset.clear (juce::dontSendNotification);
    for (size_t i = 0; i < presets.size(); ++i)
        preset.addItem (juce::String ((int) i) + "   " + presets[i].name.value_or (""), (int) i + 1);
    preset.addSeparator();
    auto file = proc.bridge().getSettings().presetsFile;
    preset.addItem ("Load presets file...", kCmdLoadPresets);
    preset.addItem ("Use built-in presets", kCmdBuiltinPresets);
    preset.setItemEnabled (kCmdBuiltinPresets, file.isNotEmpty());
    preset.setTooltip (file.isEmpty() ? juce::String ("Built-in presets") : "Presets from " + file);
    shownPresetsVersion = proc.bridge().presetsVersion();
}

void PlayPage::choosePresetsFile()
{
    auto start = proc.bridge().getSettings().presetsFile;
    chooser = std::make_unique<juce::FileChooser> ("Presets file (.toml)",
                                                   start.isNotEmpty() ? juce::File (start) : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                                                   "*.toml");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc) {
                              if (auto f = fc.getResult(); f != juce::File())
                              {
                                  auto s = proc.bridge().getSettings();
                                  s.presetsFile = f.getFullPathName();
                                  proc.bridge().setSettings (s);
                              }
                          });
}

void PlayPage::refresh (const BridgeStatus& st, bool setupDone)
{
    // presets
    if (! preset.isPopupActive())
    {
        if (shownPresetsVersion != proc.bridge().presetsVersion())
            fillPresets();
        int idx = -1;
        auto presets = proc.bridge().presets();
        for (size_t i = 0; i < presets.size(); ++i)
            if (presets[i].name.value_or ("") == st.presetName.toStdString())
                idx = (int) i;
        preset.setSelectedId (idx + 1, juce::dontSendNotification);
    }

    // parameters -> buttons
    const auto& wn = waveformNames();
    juce::String wave (wn[(size_t) juce::jlimit (0, (int) wn.size() - 1, choice ("waveform"))]);
    auto waveParts = juce::StringArray::fromTokens (wave, "+", "");
    for (int i = 0; i < 4; ++i)
        waves[(size_t) i].setSelected (waveParts.contains (kWaveNames[i]));
    const auto& fn = filterModeNames();
    juce::String mode (fn[(size_t) juce::jlimit (0, (int) fn.size() - 1, choice ("filter_mode"))]);
    auto modeParts = juce::StringArray::fromTokens (mode, "+", "");
    for (int i = 0; i < 3; ++i)
        modes[(size_t) i].setSelected (modeParts.contains (kModeNames[i]));
    velocity.setSelected (choice ("velocity"));

    // voice mode bar
    int vm = choice ("voice_mode");
    voiceModes.setSelected (vm);
    static const char* hints[] = { "Each note takes a free voice: 3 per SID.",
                                   "One voice. Overlapping notes glide (legato).",
                                   "Every voice plays the note, slightly detuned: thick.",
                                   "Each note with the octave above.",
                                   "Each note with the fifth above.",
                                   "Each note with the fifth and the octave above.",
                                   "" };
    voiceHint.setText (hints[juce::jlimit (0, 6, vm)], juce::dontSendNotification);
    arpOctaves.setSelected (juce::jlimit (0, 2, choice ("arp_octaves") - 1));
    bool hostTempo = proc.bridge().isStandalone() == false && std::abs (proc.bridge().tempoInUse() - arpBpm.getValue()) > 0.01;
    tempoLabel.setText ("Host " + juce::String (juce::roundToInt (proc.bridge().tempoInUse())) + " BPM", juce::dontSendNotification);
    bool arp = vm == 6;
    if (arp != arpShown || tempoLabel.isVisible() != (arp && hostTempo))
    {
        arpShown = arp;
        voiceHint.setVisible (! arp);
        for (juce::Component* c : { (juce::Component*) &arpRate, (juce::Component*) &arpPattern, (juce::Component*) &arpOctaves,
                                    (juce::Component*) &arpGate })
            c->setVisible (arp);
        arpBpm.setVisible (arp && ! hostTempo);
        tempoLabel.setVisible (arp && hostTempo);
        relayoutNeeded = true;
    }
    amountLabel.setText ("Amount   " + juce::String (juce::roundToInt (amount.getValue())) + "%", juce::dontSendNotification);

    auto val = [this] (Param p) { return juce::roundToInt (knobs[(size_t) p]->slider().getValue()); };
    knobs[(size_t) Param::Attack]->setHint (ui::attackTime (val (Param::Attack)));
    knobs[(size_t) Param::Decay]->setHint (ui::decayReleaseTime (val (Param::Decay)));
    knobs[(size_t) Param::Sustain]->setHint (juce::String (juce::roundToInt (val (Param::Sustain) / 15.0 * 100.0)) + "%");
    knobs[(size_t) Param::Release]->setHint (ui::decayReleaseTime (val (Param::Release)));
    graph.setAdsr (val (Param::Attack), val (Param::Decay), val (Param::Sustain), val (Param::Release));
    for (int i = 0; i < kNumParams; ++i)
    {
        bool learning = false;
        auto badge = badgeFor ("param:" + juce::String (paramName ((Param) i)), st, learning);
        knobs[(size_t) i]->setBadge (badge, learning);
    }

    // voices
    std::vector<ui::Voice> vs;
    std::map<int, int> sidIndex;
    for (auto& v : st.voices)
        sidIndex.emplace (v.base, (int) sidIndex.size() + 1);
    for (auto& v : st.voices)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        juce::String note = v.note < 0 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94"))
                                       : juce::String (names[v.note % 12]) + juce::String (v.note / 12 - 1);
        vs.push_back ({ "SID " + juce::String (sidIndex[v.base]) + kDot + juce::String (v.voice + 1), note, v.gated });
    }
    voices.setVoices (std::move (vs));

    // the line right of the presets: learn prompt / result, connection trouble, or a summary
    double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (st.learnMessage.isNotEmpty() && st.learnMessage != lastLearnMessage)
    {
        lastLearnMessage = st.learnMessage;
        learnMessageUntil = now + 5.0;
    }
    bool connected = st.state == BridgeStatus::State::connected;
    juce::String line, button;
    juce::Colour colour = label;
    if (st.learnTarget.isNotEmpty())
    {
        line = BridgeController::targetName (st.learnTarget) + ": "
             + (st.learnTarget.startsWith ("param:") ? "move a knob, slider or wheel (turn knobs clockwise)" : "press a pad or button");
        colour = warn;
        button = "Cancel";
    }
    else if (now < learnMessageUntil)
    {
        line = lastLearnMessage;
        colour = on;
    }
    else if (! connected && setupDone)
    {
        line = st.message.isNotEmpty() ? st.message : juce::String ("Not connected");
        colour = err;
        button = "Setup";
    }
    else
    {
        static const char* modeNames[] = { "Poly", "Mono", "Unison", "Octave", "Fifth", "Chord", "Arp" };
        line = juce::String (modeNames[juce::jlimit (0, 6, choice ("voice_mode"))])
             + (st.layout == "split" ? kDot + "split ch1 / ch2" : juce::String())
             + kDot + juce::String ((int) st.voices.size()) + " voices" + kDot + st.profileName;
    }
    info.setText (line, juce::dontSendNotification);
    info.setColour (juce::Label::textColourId, colour);
    bannerButton.setButtonText (button);
    bool relayout = relayoutNeeded || bannerButton.isVisible() != button.isNotEmpty();
    relayoutNeeded = false;
    bannerButton.setVisible (button.isNotEmpty());
    bannerButton.onClick = [this, button] {
        if (button == "Cancel") proc.bridge().cancelLearn();
        else if (onGoToSetup) onGoToSetup();
    };

    bool overlay = ! setupDone && ! connected;
    if (overlay != showOverlay)
    {
        showOverlay = overlay;
        goSetup.setVisible (overlay);
        goSetup.toFront (false);
        repaint();
    }
    if (relayout)
        resized();
}

void PlayPage::paintOverChildren (juce::Graphics& g)
{
    if (! showOverlay)
        return;
    auto r = getLocalBounds().reduced (20).withTrimmedTop (54).withTrimmedBottom (60);
    g.setColour (bg.withAlpha (0.88f));
    g.fillRoundedRectangle (r.toFloat(), 12.0f);
    g.setColour (bright);
    g.setFont (ui::font (18.0f, true));
    g.drawText ("Not connected yet", r.withTrimmedBottom (r.getHeight() / 2 + 10), juce::Justification::centredBottom);
    g.setColour (dim);
    g.setFont (ui::font (13.0f));
    g.drawText ("Find your C64 Ultimate on the Setup tab, then come back here to play.",
                r.withTrimmedTop (r.getHeight() / 2 - 8).withHeight (20), juce::Justification::centred);
}

void PlayPage::resized()
{
    auto r = getLocalBounds().reduced (20, 14);
    auto bar = r.removeFromTop (40);
    prevPreset.setBounds (bar.removeFromLeft (36).withSizeKeepingCentre (36, 36));
    bar.removeFromLeft (8);
    preset.setBounds (bar.removeFromLeft (280).withSizeKeepingCentre (280, 36));
    bar.removeFromLeft (8);
    nextPreset.setBounds (bar.removeFromLeft (36).withSizeKeepingCentre (36, 36));
    bar.removeFromLeft (16);
    if (bannerButton.isVisible())
    {
        bannerButton.setBounds (bar.removeFromRight (80).withSizeKeepingCentre (80, 30));
        bar.removeFromRight (8);
    }
    info.setBounds (bar);
    r.removeFromTop (12);

    voices.setBounds (r.removeFromBottom (48));
    r.removeFromBottom (10);
    layoutVoiceBar (r.removeFromBottom (36));
    r.removeFromBottom (10);

    auto row = r;
    osc.setBounds (row.removeFromLeft (196));
    row.removeFromLeft (12);
    filter.setBounds (row.removeFromLeft (228));
    row.removeFromLeft (12);
    out.setBounds (row.removeFromRight (168));
    row.removeFromRight (12);
    env.setBounds (row);

    auto knobH = [] (int d) { return d + 66; };
    auto k = [this] (Param p) -> ui::Knob& { return *knobs[(size_t) p]; };

    auto c = osc.getBounds().reduced (14).withTrimmedTop (24);
    auto chips = c.removeFromTop (94);
    int cw = (chips.getWidth() - 6) / 2;
    for (int i = 0; i < 4; ++i)
        waves[(size_t) i].setBounds (chips.getX() + (i % 2) * (cw + 6), chips.getY() + (i / 2) * 50, cw, 44);
    waveHint.setBounds (c.removeFromTop (22));
    c.removeFromTop (10);
    k (Param::PulseWidth).setBounds (c.removeFromTop (knobH (76)).withSizeKeepingCentre (110, knobH (76)));

    c = filter.getBounds().reduced (14).withTrimmedTop (24);
    auto mrow = c.removeFromTop (34);
    int mw = (mrow.getWidth() - 12) / 3;
    for (int i = 0; i < 3; ++i)
        modes[(size_t) i].setBounds (mrow.getX() + i * (mw + 6), mrow.getY(), mw, 34);
    modeHint.setBounds (c.removeFromTop (24));
    c.removeFromTop (20);
    auto fk = c.removeFromTop (knobH (76));
    k (Param::Cutoff).setBounds (fk.removeFromLeft (fk.getWidth() / 2));
    k (Param::Resonance).setBounds (fk);

    c = env.getBounds().reduced (14).withTrimmedTop (24);
    graph.setBounds (c.removeFromTop (118));
    c.removeFromTop (14);
    auto ek = c.removeFromTop (knobH (60));
    int ew = ek.getWidth() / 4;
    for (auto p : { Param::Attack, Param::Decay, Param::Sustain, Param::Release })
        k (p).setBounds (ek.removeFromLeft (ew));

    c = out.getBounds().reduced (14).withTrimmedTop (24);
    k (Param::Volume).setBounds (c.removeFromTop (knobH (76)).withSizeKeepingCentre (110, knobH (76)));
    c.removeFromTop (14);
    velocityLabel.setBounds (c.removeFromTop (18));
    velocity.setBounds (c.removeFromTop (30));
    c.removeFromTop (10);
    amountLabel.setBounds (c.removeFromTop (18));
    amount.setBounds (c.removeFromTop (22));

    auto ov = getLocalBounds().reduced (20).withTrimmedTop (54).withTrimmedBottom (60);
    goSetup.setBounds (ov.withSizeKeepingCentre (160, 40).translated (0, 40));
}

void PlayPage::layoutVoiceBar (juce::Rectangle<int> bar)
{
    voiceLabel.setBounds (bar.removeFromLeft (84));
    voiceModes.setBounds (bar.removeFromLeft (412));
    bar.removeFromLeft (14);
    voiceHint.setBounds (bar);
    arpRate.setBounds (bar.removeFromLeft (76));
    bar.removeFromLeft (6);
    arpPattern.setBounds (bar.removeFromLeft (104));
    bar.removeFromLeft (6);
    arpOctaves.setBounds (bar.removeFromLeft (112));
    bar.removeFromLeft (6);
    auto rest = bar;
    arpGate.setBounds (rest.removeFromLeft (rest.getWidth() / 2 - 3));
    rest.removeFromLeft (6);
    arpBpm.setBounds (rest);
    tempoLabel.setBounds (rest);
}
