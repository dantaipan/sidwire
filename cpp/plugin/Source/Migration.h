// SIDwire was called "SID Bridge" up to 0.1.1. The plugin and app codes stayed the same (so DAW
// projects still open), but the files under ~/Library/Application Support are named after the
// product, so the first run of SIDwire copies the old ones across, and Setup points out old
// "SID Bridge" bundles that are still installed (two copies with the same codes confuse DAWs).
#pragma once

#include <juce_core/juce_core.h>

namespace migration {

inline constexpr auto oldName = "SID Bridge";

/// For tests: where "Application Support" is (default: ~/Library/Application Support).
inline juce::File& supportDirOverride()
{
    static juce::File f;
    return f;
}

inline juce::File supportDir()
{
    if (supportDirOverride() != juce::File())
        return supportDirOverride();
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support");
}

/// Copies Application Support/<oldName + suffix> to <newName + suffix> once: only when the new one
/// doesn't exist yet. The old one stays, so going back to SID Bridge 0.1.x still works.
/// suffix is a file suffix (".settings") or a path inside the folder ("/Profiles").
inline void migrate (const juce::String& newName, const juce::String& suffix)
{
    auto from = supportDir().getChildFile (oldName + suffix);
    auto to = supportDir().getChildFile (newName + suffix);
    if (to.exists() || ! from.exists())
        return;
    to.getParentDirectory().createDirectory();
    if (from.isDirectory())
        from.copyDirectoryTo (to);
    else
        from.copyFileTo (to);
}

/// For tests: a folder that stands in for both ~ and / when looking for old bundles.
inline juce::File& bundleRootOverride()
{
    static juce::File f;
    return f;
}

/// Old "SID Bridge" app and plug-in bundles still installed in the usual places.
inline juce::Array<juce::File> oldBundles()
{
    juce::Array<juce::File> found;
    auto fake = bundleRootOverride();
    auto home = fake != juce::File() ? fake : juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    auto system = fake != juce::File() ? fake : juce::File ("/");
    const juce::String name (oldName);
    for (auto root : { home.getChildFile ("Library"), system.getChildFile ("Library") })
    {
        auto plugins = root.getChildFile ("Audio/Plug-Ins");
        for (auto f : { plugins.getChildFile ("Components/" + name + ".component"),
                        plugins.getChildFile ("VST3/" + name + ".vst3") })
            if (f.exists())
                found.addIfNotAlreadyThere (f);
    }
    for (auto f : { system.getChildFile ("Applications/" + name + ".app"), home.getChildFile ("Applications/" + name + ".app") })
        if (f.exists())
            found.addIfNotAlreadyThere (f);
    return found;
}

} // namespace migration
