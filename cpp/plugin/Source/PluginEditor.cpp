#include "PluginEditor.h"

using namespace ui::colours;

namespace {
const juce::String kDot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
}

SidBridgeEditor::SidBridgeEditor (SidBridgeProcessor& p)
    : AudioProcessorEditor (p), proc (p), play (p), setup (p)
{
    setLookAndFeel (&laf);

    title.setComponentID ("title");
    title.setTooltip ("About SIDwire");
    title.getProperties().set ("border", 0);
    title.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    title.setColour (juce::TextButton::textColourOffId, juce::Colours::transparentBlack);   // the title is painted
    title.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    title.onClick = [this] { showAbout(); };
    addAndMakeVisible (title);

    playTab.setComponentID ("tab:play");
    setupTab.setComponentID ("tab:setup");
    for (auto* t : { &playTab, &setupTab })
    {
        t->getProperties().set ("border", 0);
        t->setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        t->setColour (juce::TextButton::buttonOnColourId, selBg);
        t->setColour (juce::TextButton::textColourOffId, dim);
        addAndMakeVisible (*t);
    }
    playTab.onClick = [this] { showTab (Tab::play); };
    setupTab.onClick = [this] { showTab (Tab::setup); };

    statusPill.setComponentID ("status");
    statusPill.setTooltip ("Connection. Click for Setup.");
    statusPill.onClick = [this] { showTab (Tab::setup); };
    statusPill.setColour (juce::TextButton::buttonColourId, panel);
    statusPill.getProperties().set ("border", (int) panel.getARGB());
    statusPill.getProperties().set ("padLeft", 18);
    addAndMakeVisible (statusPill);

    stream.setComponentID ("stream");
    stream.setTooltip ("Play the C64U's own audio out of SIDwire, streamed over the network (adds about 30-40 ms). "
                       "It takes the stream over from other apps such as PS64.");
    stream.onClick = [this] {
        auto s = proc.bridge().getSettings();
        s.hearC64 = stream.getToggleState();
        proc.bridge().setSettings (s);
    };
    addAndMakeVisible (stream);

    silence.setComponentID ("silence");
    silence.setButtonText ("Silence");
    silence.setTooltip ("Stop every note on the C64 now (volume comes back with the next note). Right-click: MIDI learn.");
    silence.setColour (juce::TextButton::buttonColourId, silenceBg);
    silence.setColour (juce::TextButton::textColourOffId, silenceText);
    silence.getProperties().set ("border", (int) silenceBorder.getARGB());
    silence.onClick = [this] { proc.bridge().panic(); };
    silence.onRightClick = [this] { play.learnMenu ("action:panic", silence); };
    addAndMakeVisible (silence);

    play.onGoToSetup = [this] { showTab (Tab::setup); };
    setup.onStartPlaying = [this] { showTab (Tab::play); };
    setup.onAbout = [this] { showAbout(); };
    about.onClose = [this] { about.setVisible (false); };
    about.onCheckForUpdates = [this] { checkForUpdates(); };
    addChildComponent (about);
    updatePanel.onClose = [this] { updatePanel.setVisible (false); };
    addChildComponent (updatePanel);
    addChildComponent (play);
    addChildComponent (setup);

    setSize (960, 600);
    showTab (proc.bridge().getSettings().setupDone ? Tab::play : Tab::setup);
    timerCallback();
    startTimerHz (15);
}

SidBridgeEditor::~SidBridgeEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void SidBridgeEditor::showAbout()
{
    about.setVisible (true);
    about.toFront (true);
}

void SidBridgeEditor::checkForUpdates()
{
    updateChecker->check (true);
}

void SidBridgeEditor::showTab (Tab t)
{
    tab = t;
    play.setVisible (t == Tab::play);
    setup.setVisible (t == Tab::setup);
    playTab.setToggleState (t == Tab::play, juce::dontSendNotification);
    setupTab.setToggleState (t == Tab::setup, juce::dontSendNotification);
    if (t == Tab::setup)
        setup.autoFindIfNeeded();
}

void SidBridgeEditor::timerCallback()
{
    if (auto o = updateChecker->takeOutcome())
    {
        about.setUpdateStatus (o->error.isNotEmpty() ? o->error
                               : o->offer ? "SIDwire " + o->release->version.trimCharactersAtStart ("vV") + " is available."
                                          : "Up to date (" + updates::currentVersion() + ").");
        if (o->offer || o->manual)   // automatic checks only speak up when there's something new
        {
            about.setVisible (false);
            updatePanel.show (*o);
            updatePanel.setVisible (true);
            updatePanel.toFront (true);
        }
    }
    shown = proc.bridge().status();
    auto s = proc.bridge().getSettings();
    juce::String pill;
    switch (shown.state)
    {
        case BridgeStatus::State::connected:
            pill = "Connected" + kDot + s.host.trim() + kDot + (shown.via.startsWith ("DMA") ? "DMA" : "HTTP");
            break;
        case BridgeStatus::State::retrying:  pill = "Can't reach " + s.host.trim(); break;
        case BridgeStatus::State::lockedOut: pill = "In use by another bridge"; break;
        case BridgeStatus::State::idle:      pill = "Not connected"; break;
    }
    statusPill.setButtonText (pill);
    stream.setToggleState (s.hearC64, juce::dontSendNotification);
    if (tab == Tab::play)
        play.refresh (shown, s.setupDone);
    else
        setup.refresh (shown);
    repaint (statusPill.getBounds().expanded (2));
}

void SidBridgeEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    g.setColour (bright);
    g.setFont (ui::font (18.0f, true));
    g.drawText ("SIDwire", 20, 0, 120, 56, juce::Justification::centredLeft);
    auto tabs = playTab.getBounds().getUnion (setupTab.getBounds()).expanded (3).toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (tabs, 8.0f);
    g.setColour (juce::Colour (0xff2c3048));
    g.fillRect (0, 56, getWidth(), 1);
}

void SidBridgeEditor::paintOverChildren (juce::Graphics& g)
{
    // the status dot inside the pill
    auto b = statusPill.getBounds();
    juce::Colour dot = shown.state == BridgeStatus::State::connected ? on
                     : shown.state == BridgeStatus::State::retrying ? err
                     : shown.state == BridgeStatus::State::lockedOut ? warn : label;
    g.setColour (dot);
    g.fillEllipse ((float) b.getX() + 12.0f, (float) b.getCentreY() - 4.0f, 8.0f, 8.0f);
}

void SidBridgeEditor::resized()
{
    auto head = getLocalBounds().removeFromTop (56).reduced (20, 0);
    title.setBounds (head.removeFromLeft (120).reduced (0, 10));
    head.removeFromLeft (10);
    auto tabs = head.removeFromLeft (150).withSizeKeepingCentre (150, 30);
    playTab.setBounds (tabs.removeFromLeft (75));
    setupTab.setBounds (tabs);
    silence.setBounds (head.removeFromRight (90).withSizeKeepingCentre (90, 32));
    head.removeFromRight (16);
    stream.setBounds (head.removeFromRight (172).withSizeKeepingCentre (172, 32));
    head.removeFromRight (12);
    int pillW = juce::jmin (300, head.getWidth());
    statusPill.setBounds (head.removeFromRight (pillW).withSizeKeepingCentre (pillW, 32));
    auto body = getLocalBounds().withTrimmedTop (57);
    play.setBounds (body);
    setup.setBounds (body);
    about.setBounds (getLocalBounds());
    updatePanel.setBounds (getLocalBounds());
}
