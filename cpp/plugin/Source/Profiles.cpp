#include "Profiles.h"
#include "Migration.h"

#include "BinaryData.h"

#include <algorithm>
#include <mutex>

namespace {

std::mutex dirMutex;
juce::File testDir;

std::vector<ProfileEntry> builtins()
{
    std::vector<ProfileEntry> out;
    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        juce::String file (BinaryData::originalFilenames[i]);
        if (! file.endsWithIgnoreCase (".toml") || file == "presets.toml")
            continue;
        int size = 0;
        auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size);
        ProfileEntry e;
        e.id = file.upToLastOccurrenceOf (".toml", false, true);
        e.origin = "built-in";
        e.text.assign (data, (size_t) size);
        out.push_back (e);
    }
    return out;
}

void describe (ProfileEntry& e)
{
    try
    {
        e.name = msid::parseProfile (e.text, (e.origin == "built-in" ? e.id + ".toml" : e.origin).toStdString()).name;
    }
    catch (const std::exception& ex)
    {
        e.error = ex.what();
        e.name = e.id + " (invalid - see its file)";
    }
}

} // namespace

juce::File Profiles::userDir()
{
    std::lock_guard g (dirMutex);
    if (testDir != juce::File())
        return testDir;
    migration::migrate ("SIDwire", "/Profiles");   // from SID Bridge 0.1.x
    return migration::supportDir().getChildFile ("SIDwire").getChildFile ("Profiles");
}

void Profiles::setUserDirForTests (const juce::File& dir)
{
    std::lock_guard g (dirMutex);
    testDir = dir;
}

std::vector<ProfileEntry> Profiles::list()
{
    auto out = builtins();
    for (auto& f : userDir().findChildFiles (juce::File::findFiles, false, "*.toml"))
    {
        ProfileEntry e;
        e.id = f.getFileNameWithoutExtension();
        e.origin = f.getFullPathName();
        e.text = f.loadFileAsString().toStdString();
        auto same = std::find_if (out.begin(), out.end(), [&] (auto& b) { return b.id == e.id; });
        if (same != out.end())
            *same = e;   // the user's copy wins
        else
            out.push_back (e);
    }
    for (auto& e : out)
        describe (e);
    std::sort (out.begin(), out.end(), [] (auto& a, auto& b) {
        if ((a.id == "generic") != (b.id == "generic"))
            return a.id == "generic";
        return a.name.compareNatural (b.name) < 0;
    });
    return out;
}

msid::Profile Profiles::load (const juce::String& id, juce::String& error)
{
    error = {};
    for (auto& e : list())
        if (e.id == id)
        {
            if (e.error.isNotEmpty())
            {
                error = e.error;
                return msid::genericProfile();
            }
            return msid::parseProfile (e.text, e.origin.toStdString());
        }
    if (id.isNotEmpty() && id != "generic")
        error = "controller profile '" + id + "' not found";
    return msid::genericProfile();
}

juce::File Profiles::prepareUserDir()
{
    auto dir = userDir();
    dir.createDirectory();
    auto readme = dir.getChildFile ("README.txt");
    if (! readme.existsAsFile())
        readme.replaceWithText (
            "SIDwire controller profiles\n"
            "==============================\n\n"
            "Each .toml file here is a controller profile: it tells SIDwire what your MIDI\n"
            "keyboard's knobs, faders, pads and buttons should do. Pick one in SIDwire's\n"
            "Profile box.\n\n"
            "To make your own: copy generic.toml.example to my-keyboard.toml, change the name\n"
            "and the [[control]] entries (the format is explained at the top of the file), then\n"
            "choose it in the Profile box. A file named like a built-in profile (generic.toml,\n"
            "oxygen-49-mkv.toml) replaces that built-in.\n");
    for (auto& b : builtins())
    {
        auto f = dir.getChildFile (b.id + ".toml.example");
        f.replaceWithText (juce::String::fromUTF8 (b.text.data(), (int) b.text.size()));   // always current
    }
    return dir;
}
