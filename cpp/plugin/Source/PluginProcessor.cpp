#include "PluginProcessor.h"

#include "BinaryData.h"
#include "PluginEditor.h"

using namespace msid;

namespace {
int defaultFor (Param p)
{
    Patch d;
    return d.get (p);
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout SidBridgeProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const char* labels[kNumParams] = { "Pulse Width", "Cutoff", "Resonance", "Attack",
                                       "Decay", "Sustain", "Release", "Volume" };
    for (int i = 0; i < kNumParams; ++i)
    {
        auto p = (Param) i;
        layout.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { paramName (p), 1 },
                                                               labels[i], 0, paramMax (p), defaultFor (p)));
    }
    juce::StringArray waves, modes;
    for (auto& w : waveformNames()) waves.add (w);
    for (auto& m : filterModeNames()) modes.add (m);
    Patch d;
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "waveform", 1 }, "Waveform",
                                                              waves, waves.indexOf (d.waveform)));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "filter_mode", 1 }, "Filter Mode",
                                                              modes, modes.indexOf (d.filterMode)));
    EngineConfig ec;
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "velocity", 1 }, "Velocity",
                                                              velocityModes(), velocityModes().indexOf (ec.velocity)));
    layout.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { "velocity_amount", 1 }, "Velocity Amount",
                                                           0, 100, (int) std::lround (ec.velocityAmount * 100),
                                                           juce::AudioParameterIntAttributes().withLabel ("%")));
    return layout;
}

std::vector<Preset> SidBridgeProcessor::builtinPresets()
{
    try
    {
        return parsePresets (std::string (BinaryData::presets_toml, (size_t) BinaryData::presets_tomlSize),
                             "built-in presets.toml");
    }
    catch (const std::exception& e)
    {
        DBG ("presets: " << e.what());
        return {};
    }
}

static BridgeParams makeParams (juce::AudioProcessorValueTreeState& s)
{
    BridgeParams bp;
    for (int i = 0; i < kNumParams; ++i)
        bp.ints[(size_t) i] = s.getParameter (paramName ((Param) i));
    bp.waveform = s.getParameter ("waveform");
    bp.filterMode = s.getParameter ("filter_mode");
    bp.velocity = s.getParameter ("velocity");
    bp.velocityAmount = s.getParameter ("velocity_amount");
    return bp;
}

SidBridgeProcessor::SidBridgeProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    controller = std::make_unique<BridgeController> (makeParams (apvts), builtinPresets());
    controller->setStandalone (wrapperType == wrapperType_Standalone);
    controller->setSettings (withEnvOverrides ({}));
    startTimerHz (30);
}

SidBridgeProcessor::~SidBridgeProcessor()
{
    stopTimer();
    controller.reset();   // panics and releases the C64U before the parameters go away
}

/// Development / test hook: MSID_HOST, MSID_PASSWORD, MSID_NOTE_PORT, MSID_ENCODER_PORT,
/// MSID_LAYOUT, MSID_TRANSPORT, MSID_HEAR_C64, MSID_PROFILE override the saved settings (used to drive the
/// standalone app from scripts).
BridgeSettings SidBridgeProcessor::withEnvOverrides (BridgeSettings s)
{
    auto env = [] (const char* name) { return juce::SystemStats::getEnvironmentVariable (name, {}); };
    if (auto v = env ("MSID_HOST"); v.isNotEmpty()) s.host = v;
    if (auto v = env ("MSID_PASSWORD"); v.isNotEmpty()) s.password = v;
    if (auto v = env ("MSID_NOTE_PORT"); v.isNotEmpty()) s.notePort = v;
    if (auto v = env ("MSID_ENCODER_PORT"); v.isNotEmpty()) s.encoderPort = v;
    if (auto v = env ("MSID_LAYOUT"); v.isNotEmpty()) s.layout = v;
    if (auto v = env ("MSID_TRANSPORT"); v.isNotEmpty()) s.transport = v;
    if (auto v = env ("MSID_HEAR_C64"); v.isNotEmpty()) s.hearC64 = v == "1";
    if (auto v = env ("MSID_PROFILE"); v.isNotEmpty()) s.profile = v;
    return s;
}

bool SidBridgeProcessor::envOverridesActive()
{
    for (auto* n : { "MSID_HOST", "MSID_PASSWORD", "MSID_NOTE_PORT", "MSID_ENCODER_PORT", "MSID_LAYOUT",
                     "MSID_TRANSPORT", "MSID_HEAR_C64", "MSID_PROFILE" })
        if (juce::SystemStats::getEnvironmentVariable (n, {}).isNotEmpty())
            return true;
    return false;
}

bool SidBridgeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void SidBridgeProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    controller->audioStream().prepare (sampleRate, blockSize);
}

void SidBridgeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();   // the sound comes out of the C64 ...
    controller->audioStream().render (buffer, 0, buffer.getNumSamples());   // ... and back, if "C64 audio" is on
    for (const auto meta : midi)
        controller->pushHostMidi (meta.getMessage());
    midi.clear();
}

int SidBridgeProcessor::getNumPrograms()
{
    return juce::jmax (1, (int) controller->presets().size());
}

void SidBridgeProcessor::setCurrentProgram (int index)
{
    if (controller->presets().empty())
        return;
    currentProgram = juce::jlimit (0, getNumPrograms() - 1, index);
    controller->selectPreset (currentProgram);
}

const juce::String SidBridgeProcessor::getProgramName (int index)
{
    const auto& p = controller->presets();
    if (index < 0 || index >= (int) p.size())
        return {};
    return p[(size_t) index].name.value_or ("");
}

void SidBridgeProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.removeChild (state.getChildWithName ("Bridge"), nullptr);
    // test/script runs (MSID_* overrides) must not leave their settings in the user's saved state
    auto settings = envOverridesActive() ? stateSettings : controller->getSettings();
    state.appendChild (settings.toValueTree(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void SidBridgeProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;
    auto state = juce::ValueTree::fromXml (*xml);
    auto bridgeTree = state.getChildWithName ("Bridge");
    apvts.replaceState (state);
    stateSettings = BridgeSettings::fromValueTree (bridgeTree);
    controller->setSettings (withEnvOverrides (stateSettings));
}

juce::AudioProcessorEditor* SidBridgeProcessor::createEditor()
{
    return new SidBridgeEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SidBridgeProcessor();
}
