// Controller profiles in the plugin: the built-in ones (profiles/*.toml, compiled in) plus the
// user's own files in ~/Library/Application Support/SID Bridge/Profiles. A user file with the
// same name (file stem) as a built-in replaces it. Format: profiles/generic.toml.
#pragma once

#include <juce_core/juce_core.h>

#include "msid/Profile.h"

#include <vector>

struct ProfileEntry
{
    juce::String id;       // file stem, what the settings store
    juce::String name;     // the profile's name (or a note that it is invalid)
    juce::String origin;   // "built-in" or the file path
    juce::String error;    // non-empty: the file doesn't parse
    std::string text;
};

namespace Profiles
{
    juce::File userDir();
    /// Tests: use another folder for user profiles (an invalid File = back to the default).
    void setUserDirForTests (const juce::File&);
    /// Built-ins, then user files; sorted by name with the generic profile first.
    std::vector<ProfileEntry> list();
    /// The profile with this id. On failure: the generic profile, and error says why.
    msid::Profile load (const juce::String& id, juce::String& error);
    /// Creates the user folder with a README and the built-ins as *.toml.example; returns it.
    juce::File prepareUserDir();
}
