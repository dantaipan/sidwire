#include "SetupPage.h"

using namespace msid;
using namespace ui::colours;

namespace {
const juce::String kArrow (juce::CharPointer_UTF8 (" \xe2\x86\x92 "));
const juce::String kDot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
} // namespace

SetupPage::SetupPage (SidBridgeProcessor& p) : proc (p)
{
    for (auto* c : { &c64, &keyboard, &playing })
        addAndMakeVisible (*c);
    c64.setStep (1, false);
    keyboard.setStep (2, false);
    playing.setStep (3, false, "(optional)");

    for (auto* l : { &addressLabel, &passwordLabel, &basesLabel, &viaLabel, &videoLabel, &profileLabel, &notePortLabel,
                     &encoderPortLabel, &layoutLabel, &targetLabel, &presetsLabel })
    {
        l->setFont (ui::font (11.5f));
        l->setColour (juce::Label::textColourId, dim);
        l->setBorderSize ({});
        addAndMakeVisible (*l);
    }
    for (auto* l : { &findText, &c64Info, &mappings, &presetsName })
    {
        l->setFont (ui::font (l == &mappings ? 11.5f : 12.0f));
        l->setColour (juce::Label::textColourId, juce::Colour (0xffc3c8e3));
        l->setJustificationType (l == &presetsName ? juce::Justification::centredLeft : juce::Justification::topLeft);
        l->setBorderSize ({ 2, 2, 2, 2 });
        addAndMakeVisible (*l);
    }

    // 1: C64 Ultimate
    host.setComponentID ("host");
    host.setTextToShowWhenEmpty ("192.168.x.x", label);
    password.setPasswordCharacter ((juce::juce_wchar) 0x2022);
    password.setTextToShowWhenEmpty ("none", label);
    bases.setTooltip ("SID base addresses, e.g. D400,D420");
    for (auto* t : { &host, &password, &bases })
    {
        t->setFont (ui::font (13.0f));
        t->setIndents (10, 8);
        t->onReturnKey = [this] { pushSettings(); };
        t->onFocusLost = [this] { pushSettings(); };
        addAndMakeVisible (*t);
    }
    find.setComponentID ("find");
    find.setTooltip ("Search your network for C64 Ultimates (about 3 seconds)");
    find.getProperties().set ("border", (int) selBorder.getARGB());
    find.setColour (juce::TextButton::buttonColourId, selBg);
    find.setColour (juce::TextButton::textColourOffId, bright);
    find.onClick = [this] { startFind(); };
    addAndMakeVisible (find);
    more.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffaab3ff));
    more.getProperties().set ("border", 0);
    more.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    more.onClick = [this] { showMore = ! showMore; resized(); };
    more.setTooltip ("Network password, SID addresses, DMA or HTTP, PAL or NTSC");
    addAndMakeVisible (more);
    via.onChange = [this] (int) { pushSettings(); };
    video.onChange = [this] (int) { pushSettings(); };
    via.setTooltip ("DMA needs \"Ultimate DMA Service\" on in the C64U's Network Settings; without it SID Bridge uses HTTP by itself");
    addAndMakeVisible (via);
    addAndMakeVisible (video);
    connectButton.onClick = [this] {
        auto s = proc.bridge().getSettings();
        s.enabled = ! s.enabled;
        proc.bridge().setSettings (s);
    };
    addAndMakeVisible (connectButton);

    // 2: keyboard
    profileBox.setComponentID ("profile");
    profileBox.setTooltip ("Controller profile: what your keyboard's knobs, pads and buttons do");
    profileBox.onChange = [this] {
        if (profileBox.getSelectedId() == kCmdOpenProfiles)
        {
            Profiles::prepareUserDir().revealToUser();
            fillProfiles();
            return;
        }
        pushSettings();
    };
    notePort.setComponentID ("notePort");
    encoderPort.setComponentID ("encoderPort");
    notePort.setTooltip ("Open a keyboard directly (lowest latency in the standalone app). While set, MIDI from the host is ignored.");
    encoderPort.setTooltip ("A second input for knobs and buttons, e.g. a DAW / Mackie port. Auto = the one named in the profile.");
    for (auto* c : { &profileBox, &notePort, &encoderPort })
    {
        if (c != &profileBox)
            c->onChange = [this] { pushSettings(); };
        addAndMakeVisible (*c);
    }
    openFolder.onClick = [] { Profiles::prepareUserDir().revealToUser(); };
    addAndMakeVisible (openFolder);

    // 3: how you play
    layout.onChange = [this] (int) { pushSettings(); };
    target.onChange = [this] (int) { pushSettings(); };
    target.setTooltip ("In split layout: which SID the controls-port knobs and the pads change");
    for (auto* c : { &layout, &target })
        addAndMakeVisible (*c);
    loadPresets.onClick = [this] { choosePresetsFile(); };
    builtinPresets.onClick = [this] {
        auto s = proc.bridge().getSettings();
        s.presetsFile = {};
        proc.bridge().setSettings (s);
    };
    addAndMakeVisible (loadPresets);
    addAndMakeVisible (builtinPresets);

    audioOut.setComponentID ("audioOutput");
    audioOut.setTooltip ("Your audio interface: where Stream C64U audio plays. SID Bridge makes no other sound: the C64U does.");
    audioOut.onChange = [this] {
        if (auto* dm = standalone::deviceManager(); dm != nullptr && audioOut.getSelectedId() > 0)
        {
            auto setup = dm->getAudioDeviceSetup();
            setup.outputDeviceName = audioOut.getText();
            dm->setAudioDeviceSetup (setup, true);
        }
    };
    audioOutLabel.setFont (ui::font (11.5f));
    audioOutLabel.setColour (juce::Label::textColourId, dim);
    audioOutLabel.setBorderSize ({});
    bool standaloneApp = standalone::deviceManager() != nullptr;
    addChildComponent (audioOutLabel);
    addChildComponent (audioOut);
    audioOutLabel.setVisible (standaloneApp);
    audioOut.setVisible (standaloneApp);
    if (standaloneApp)
        fillAudioOutputs();

    footnote.setComponentID ("footnote");
    footnote.setButtonText ("Made with JUCE" + kDot + "About SID Bridge");
    footnote.setTooltip ("Version, and how SID Bridge and JUCE are licensed");
    footnote.getProperties().set ("border", 0);
    footnote.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    footnote.setColour (juce::TextButton::textColourOffId, label);
    footnote.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    footnote.onClick = [this] { if (onAbout) onAbout(); };
    addAndMakeVisible (footnote);

    startPlaying.setComponentID ("startPlaying");
    startPlaying.setButtonText (juce::String ("Start playing") + juce::String (juce::CharPointer_UTF8 (" \xe2\x86\x92")));
    startPlaying.setColour (juce::TextButton::buttonColourId, on);
    startPlaying.setColour (juce::TextButton::textColourOffId, onText);
    startPlaying.getProperties().set ("border", (int) on.getARGB());
    startPlaying.onClick = [this] { if (onStartPlaying) onStartPlaying(); };
    addChildComponent (startPlaying);

    loadSettings();
}

SetupPage::~SetupPage() = default;

void SetupPage::fillAudioOutputs()
{
    auto* dm = standalone::deviceManager();
    if (dm == nullptr || audioOut.isPopupActive())
        return;
    juce::StringArray names;
    if (auto* type = dm->getCurrentDeviceTypeObject())
        names = type->getDeviceNames (false);
    auto current = dm->getAudioDeviceSetup().outputDeviceName;
    audioOut.clear (juce::dontSendNotification);
    for (int i = 0; i < names.size(); ++i)
        audioOut.addItem (names[i], i + 1);
    audioOut.setSelectedId (names.indexOf (current) + 1, juce::dontSendNotification);
    audioOut.setTextWhenNothingSelected (names.isEmpty() ? "No audio outputs" : "Choose an output");
}

juce::String SetupPage::notesNoneLabel() const
{
    if (! proc.bridge().isStandalone())
        return "From the host / app";
    auto open = proc.bridge().openNotesPort();
    return open.isNotEmpty() ? "Auto: " + open : juce::String ("Auto (from the profile)");
}

// ---------------------------------------------------------------- settings

void SetupPage::fillPortBox (juce::ComboBox& box, const juce::String& noneLabel, const juce::String& current)
{
    box.clear (juce::dontSendNotification);
    box.addItem (noneLabel, 1);
    int id = 2, selected = 1;
    for (auto& d : juce::MidiInput::getAvailableDevices())
    {
        box.addItem (d.name, id);
        if (d.name == current)
            selected = id;
        ++id;
    }
    if (current.isNotEmpty() && selected == 1)   // saved port not present right now: keep it listed
    {
        box.addItem (current + " (not connected)", id);
        selected = id;
    }
    box.setSelectedId (selected, juce::dontSendNotification);
}

juce::String SetupPage::portFrom (const juce::ComboBox& box) const
{
    if (box.getSelectedId() <= 1)
        return {};
    return box.getText().upToLastOccurrenceOf (" (not connected)", false, false);
}

void SetupPage::loadSettings()
{
    auto s = proc.bridge().getSettings();
    host.setText (s.host, false);
    password.setText (s.password, false);
    bases.setText (s.bases, false);
    via.setSelected (s.transport == "http" ? 1 : 0);
    video.setSelected (s.video == "NTSC" ? 1 : 0);
    layout.setSelected (s.layout == "split" ? 1 : 0);
    target.setSelected (juce::jlimit (0, 2, s.knobsTarget));
    fillPortBox (notePort, notesNoneLabel(), s.notePort);
    fillPortBox (encoderPort, "Auto (from the profile)", s.encoderPort);
    fillProfiles();
    knownPorts.clear();
    for (auto& d : juce::MidiInput::getAvailableDevices())
        knownPorts.add (d.name);
}

void SetupPage::pushSettings()
{
    auto s = proc.bridge().getSettings();   // keeps what this page doesn't show
    s.host = host.getText().trim();
    s.password = password.getText();
    s.bases = bases.getText().trim();
    s.transport = via.selected() == 1 ? "http" : "dma";
    s.video = video.selected() == 1 ? "NTSC" : "PAL";
    s.layout = layout.selected() == 1 ? "split" : "poly";
    s.knobsTarget = target.selected();
    s.notePort = portFrom (notePort);
    s.encoderPort = portFrom (encoderPort);
    if (auto id = selectedProfile(); id.isNotEmpty())
        s.profile = id;
    proc.bridge().setSettings (s);
}

juce::String SetupPage::selectedProfile() const
{
    auto i = profileBox.getSelectedId() - 1;
    return i >= 0 && i < (int) shownProfiles.size() ? shownProfiles[(size_t) i].id : juce::String();
}

void SetupPage::fillProfiles()
{
    auto list = Profiles::list();
    auto current = proc.bridge().getSettings().profile;
    profileBox.clear (juce::dontSendNotification);
    int selected = 0;
    for (size_t i = 0; i < list.size(); ++i)
    {
        profileBox.addItem (list[i].name + (list[i].origin == "built-in" ? "" : "  (yours)"), (int) i + 1);
        profileBox.setItemEnabled ((int) i + 1, list[i].error.isEmpty());
        if (list[i].id == current)
            selected = (int) i + 1;
    }
    if (selected == 0 && current.isNotEmpty())
    {
        ProfileEntry missing;
        missing.id = current;
        missing.name = current + " (missing)";
        list.push_back (missing);
        profileBox.addItem (missing.name, (int) list.size());
        selected = (int) list.size();
    }
    profileBox.addSeparator();
    profileBox.addItem ("Open profiles folder...", kCmdOpenProfiles);
    profileBox.setSelectedId (selected, juce::dontSendNotification);
    shownProfiles = std::move (list);
}

void SetupPage::refreshPorts()
{
    juce::StringArray now;
    for (auto& d : juce::MidiInput::getAvailableDevices())
        now.add (d.name);
    if (now == knownPorts || notePort.isPopupActive() || encoderPort.isPopupActive())
        return;
    knownPorts = now;
    auto s = proc.bridge().getSettings();
    fillPortBox (notePort, notesNoneLabel(), s.notePort);
    fillPortBox (encoderPort, "Auto (from the profile)", s.encoderPort);
}

void SetupPage::choosePresetsFile()
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

// ---------------------------------------------------------------- finding the C64U

void SetupPage::autoFindIfNeeded()
{
    auto s = proc.bridge().getSettings();
    if (! s.setupDone && s.host.trim().isEmpty() && ! proc.bridge().findState().running)
    {
        autoUse = true;
        startFind();
    }
}

void SetupPage::startFind()
{
    proc.bridge().startFind();
    findWasRunning = true;
    showFound (proc.bridge().findState());
}

void SetupPage::useDevice (const FoundDevice& d)
{
    host.setText (d.ip, false);
    auto s = proc.bridge().getSettings();
    s.host = d.ip;
    s.enabled = true;
    proc.bridge().setSettings (s);
    if (d.locked)
    {
        showMore = true;   // it needs the network password
        resized();
        password.grabKeyboardFocus();
    }
}

void SetupPage::showFound (const BridgeController::FindState& f)
{
    bool changed = f.found != found;
    found = f.found;
    if (changed)
    {
        foundButtons.clear();
        for (auto& d : found)
        {
            auto b = std::make_unique<juce::TextButton>();
            b->setButtonText (juce::String (d.hostname.empty() ? d.ip : d.hostname) + "   " + juce::String (d.ip)
                              + (d.locked ? juce::String ("  (password protected)") : kDot + "firmware " + juce::String (d.firmware)));
            b->setComponentID ("found:" + juce::String (d.ip));
            b->onClick = [this, d] { useDevice (d); };
            addAndMakeVisible (*b);
            foundButtons.push_back (std::move (b));
        }
        resized();
    }
    if (f.running)
        findText.setText ("Searching your network... " + juce::String (f.progress) + "%", juce::dontSendNotification);
    else if (f.finished && found.empty())
        findText.setText ("No C64 Ultimate found. Is it on this network, with Web Remote Control on "
                          "(C64U menu > Network Settings)? You can also type its address.", juce::dontSendNotification);
    else if (f.finished)
        findText.setText ("Found on your network:", juce::dontSendNotification);
    else
        findText.setText ("Press Find to search your network, or type the address shown in the C64U's network menu.",
                          juce::dontSendNotification);
    find.setEnabled (! f.running);
}

// ---------------------------------------------------------------- refresh / layout

void SetupPage::refresh (const BridgeStatus& st)
{
    if (++ticks % 15 == 0)
    {
        refreshPorts();
        fillAudioOutputs();
        if (! notePort.isPopupActive() && notePort.getItemText (0) != notesNoneLabel())
            notePort.changeItemText (1, notesNoneLabel());
        if (! profileBox.isPopupActive())
        {
            auto key = [] (const std::vector<ProfileEntry>& l) {
                juce::StringArray k;
                for (auto& e : l)
                    if (e.origin.isNotEmpty())
                        k.add (e.id + "|" + e.name);
                return k;
            };
            if (key (Profiles::list()) != key (shownProfiles) || selectedProfile() != proc.bridge().getSettings().profile)
                fillProfiles();
        }
    }
    auto f = proc.bridge().findState();
    showFound (f);
    if (findWasRunning && f.finished)
    {
        findWasRunning = false;
        int usable = 0;
        for (auto& d : f.found)
            usable += d.locked ? 0 : 1;
        if (autoUse && f.found.size() == 1 && usable == 1 && host.getText().trim().isEmpty())
            useDevice (f.found[0]);   // first run, exactly one: just use it
        autoUse = false;
    }

    auto s = proc.bridge().getSettings();
    if (! host.hasKeyboardFocus (true) && host.getText().trim() != s.host)
        host.setText (s.host, false);
    bool connected = st.state == BridgeStatus::State::connected;
    c64.setStep (1, connected);
    keyboard.setStep (2, connected);
    connectButton.setButtonText (s.enabled ? "Disconnect" : "Connect");
    more.setButtonText (showMore ? juce::String ("Fewer settings") : juce::String ("More settings"));

    juce::String info;
    info << "Sending via " << (st.via.isEmpty() ? juce::String ("-") : st.via) << "\n"
         << "Writes / s " << (int) st.writesPerSec << kDot << "errors " << st.errors;
    if (st.audio.isNotEmpty())
        info << "\n" << st.audio;
    c64Info.setText (info, juce::dontSendNotification);

    // one line per thing controlled: "Cutoff: CC16 ctl, CC74"
    juce::StringArray targets;
    std::map<juce::String, juce::StringArray> triggers;
    for (auto port : { Port::Controls, Port::Notes })
        for (auto& c : st.profile.controls)
            if (c.port == port)
            {
                auto t = c.param.empty() ? "action:" + juce::String (c.action) : "param:" + juce::String (c.param);
                targets.addIfNotAlreadyThere (t);
                triggers[t].add (shortTrigger (c));
            }
    juce::StringArray params, actions;
    for (int i = 0; i < kNumParams; ++i)
        if (targets.contains ("param:" + juce::String (paramName ((Param) i))))
            params.add ("param:" + juce::String (paramName ((Param) i)));
    for (auto& t : targets)
        if (t.startsWith ("action:"))
            actions.add (t);
    juce::String map;
    for (auto& t : params)
        map << BridgeController::targetName (t) << ": " << triggers[t].joinIntoString (", ") << "\n";
    for (auto& t : actions)
        map << BridgeController::targetName (t) << ": " << triggers[t].joinIntoString (", ") << "\n";
    if (! st.profile.ignoreChannels.empty())
        map << "Notes on channel " << st.profile.ignoreChannels[0] << ": never played\n";
    if (map.isEmpty())
        map = "Nothing mapped yet: right-click a knob on the Play tab to MIDI learn it.";
    mappings.setText ("What this profile does\n" + map, juce::dontSendNotification);

    auto presetsFile = s.presetsFile;
    presetsName.setText (presetsFile.isEmpty() ? "Built-in (" + juce::String ((int) proc.bridge().presets().size()) + ")"
                                               : juce::File (presetsFile).getFileName(),
                         juce::dontSendNotification);
    builtinPresets.setEnabled (presetsFile.isNotEmpty());

    bool nowReady = connected;
    readyTitle = connected ? juce::String ("Ready to play") : juce::String ("Not connected yet");
    readyLine = connected ? "Connected to " + s.host + ". Play a key: the voices light up on the Play tab."
                          : (st.message.isNotEmpty() ? st.message : juce::String ("Enter your C64U's address or press Find."));
    if (st.notice.isNotEmpty())
        readyLine << "   ! " << st.notice;
    if (nowReady != ready)
    {
        ready = nowReady;
        startPlaying.setVisible (ready);
    }
    repaint (getLocalBounds().removeFromBottom (80));
}

void SetupPage::paint (juce::Graphics& g)
{
    auto bar = getLocalBounds().reduced (20, 0).removeFromBottom (76).withTrimmedBottom (16).toFloat();
    g.setColour (ready ? readyBg : panel);
    g.fillRoundedRectangle (bar, 12.0f);
    g.setColour (ready ? readyBorder : border);
    g.drawRoundedRectangle (bar.reduced (0.5f), 12.0f, 1.0f);
    auto r = bar.toNearestInt().reduced (16, 10);
    auto dot = r.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat();
    g.setColour (ready ? on : warn);
    g.fillEllipse (dot);
    r.removeFromLeft (12);
    if (ready)
        r.removeFromRight (180);
    g.setColour (bright);
    g.setFont (ui::font (14.0f, true));
    g.drawText (readyTitle, r.removeFromTop (r.getHeight() / 2), juce::Justification::bottomLeft);
    g.setColour (ready ? readyText : dim);
    g.setFont (ui::font (12.0f));
    g.drawFittedText (readyLine, r, juce::Justification::topLeft, 1);
}

void SetupPage::resized()
{
    auto r = getLocalBounds().reduced (20, 16);
    auto bar = r.removeFromBottom (60);
    startPlaying.setBounds (bar.removeFromRight (180).reduced (10, 10));
    r.removeFromBottom (12);
    int w = (r.getWidth() - 24) / 3;
    c64.setBounds (r.removeFromLeft (w));
    r.removeFromLeft (12);
    keyboard.setBounds (r.removeFromLeft (w));
    r.removeFromLeft (12);
    playing.setBounds (r);

    auto place = [] (juce::Rectangle<int>& c, juce::Label& l, juce::Component& comp, int h = 34) {
        l.setBounds (c.removeFromTop (16));
        c.removeFromTop (4);
        comp.setBounds (c.removeFromTop (h));
        c.removeFromTop (10);
    };

    // 1
    auto c = c64.content();
    addressLabel.setBounds (c.removeFromTop (16));
    c.removeFromTop (4);
    auto row = c.removeFromTop (36);
    find.setBounds (row.removeFromRight (76));
    row.removeFromRight (6);
    host.setBounds (row);
    c.removeFromTop (8);
    auto bottom = c.removeFromBottom (34);
    connectButton.setBounds (bottom);
    c.removeFromBottom (10);
    if (! showMore)
    {
        findText.setBounds (c.removeFromTop (foundButtons.empty() ? 52 : 20));
        for (auto& b : foundButtons)
        {
            b->setVisible (c.getHeight() >= 40);
            b->setBounds (c.removeFromTop (40));
            c.removeFromTop (4);
        }
        c.removeFromTop (4);
        more.setBounds (c.removeFromTop (24));
        c64Info.setBounds (c.removeFromBottom (56));
        for (auto* comp : std::initializer_list<juce::Component*> { &passwordLabel, &password, &basesLabel, &bases, &viaLabel, &via, &videoLabel, &video })
            comp->setVisible (false);
        findText.setVisible (true);
        c64Info.setVisible (true);
    }
    else
    {
        findText.setVisible (false);
        c64Info.setVisible (false);
        for (auto& b : foundButtons)
            b->setVisible (false);
        more.setBounds (c.removeFromTop (24));
        for (auto* comp : std::initializer_list<juce::Component*> { &passwordLabel, &password, &basesLabel, &bases, &viaLabel, &via, &videoLabel, &video })
            comp->setVisible (true);
        place (c, passwordLabel, password);
        place (c, basesLabel, bases);
        auto halves = c.removeFromTop (50);
        auto left = halves.removeFromLeft (halves.getWidth() / 2 - 4);
        halves.removeFromLeft (8);
        place (left, viaLabel, via, 30);
        place (halves, videoLabel, video, 30);
    }

    // 2
    c = keyboard.content();
    place (c, profileLabel, profileBox);
    place (c, notePortLabel, notePort);
    place (c, encoderPortLabel, encoderPort);
    openFolder.setBounds (c.removeFromBottom (32));
    c.removeFromBottom (8);
    mappings.setBounds (c);

    // 3
    c = playing.content();
    place (c, layoutLabel, layout, 30);
    place (c, targetLabel, target, 30);
    presetsLabel.setBounds (c.removeFromTop (16));
    c.removeFromTop (4);
    auto prow = c.removeFromTop (32);
    builtinPresets.setBounds (prow.removeFromRight (74));
    prow.removeFromRight (6);
    loadPresets.setBounds (prow.removeFromRight (90));
    prow.removeFromRight (6);
    presetsName.setBounds (prow);
    c.removeFromTop (14);
    place (c, audioOutLabel, audioOut);
    footnote.setBounds (playing.getBounds().reduced (6, 8).removeFromBottom (24));
}
