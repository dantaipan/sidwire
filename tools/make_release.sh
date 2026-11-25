#!/bin/bash
# Builds SIDwire for distribution: the standalone app, the Audio Unit and the VST3, universal
# (Apple Silicon + Intel), signed with Developer ID (hardened runtime), notarized and stapled,
# inside a signed, notarized and stapled disk image: dist/SIDwire-<version>.dmg.
#
# Needs Xcode command line tools, CMake + Ninja, a "Developer ID Application" certificate in the
# keychain, and notarytool credentials saved under a keychain profile:
#   xcrun notarytool store-credentials <profile> --apple-id ... --team-id ...
# The profile is $NOTARY_PROFILE, else roughtrack-notary, else rifftrack-notary (shared with
# RoughTrack's release script).
#
# The version is the CMake project version (cpp/CMakeLists.txt), which the About box and the
# update checker use. Publish the disk image on github.com/dantaipan/sidwire as release
# v<version> so the update checker finds it.

set -euo pipefail

identity="Developer ID Application"
profile="${NOTARY_PROFILE:-}"
if [ -z "$profile" ]; then
    for p in roughtrack-notary rifftrack-notary; do
        if xcrun notarytool history --keychain-profile "$p" >/dev/null 2>&1; then
            profile="$p"
            break
        fi
    done
fi
if [ -z "$profile" ]; then
    echo "No notarytool credentials found: set NOTARY_PROFILE (see the top of this script)." >&2
    exit 1
fi

cd "$(dirname "$0")/.."
root="$(pwd)"
version=$(sed -nE 's/^project\(MidiSid VERSION ([0-9.]+).*/\1/p' cpp/CMakeLists.txt)
out="$root/dist"
work="$out/work"
artefacts="$root/cpp/build/dist/plugin/SidBridge_artefacts/Release"

if [ ! -f cpp/external/JUCE/CMakeLists.txt ]; then
    echo "JUCE is missing: git submodule update --init" >&2
    exit 1
fi

rm -rf "$work"
mkdir -p "$work"

echo "== Building SIDwire $version (universal)"
(cd cpp && cmake --preset dist >/dev/null && cmake --build --preset dist)

app="$work/SIDwire.app"
au="$work/SIDwire.component"
vst3="$work/SIDwire.vst3"
ditto "$artefacts/Standalone/SIDwire.app" "$app"
ditto "$artefacts/AU/SIDwire.component" "$au"
ditto "$artefacts/VST3/SIDwire.vst3" "$vst3"

echo "== Signing"
for bundle in "$app" "$au" "$vst3"; do
    codesign --force --options runtime --timestamp --sign "$identity" "$bundle"
    codesign --verify --strict --verbose=1 "$bundle"
done
codesign --display --verbose=2 "$app" 2>&1 | grep -E "Authority=Developer ID Application|Timestamp=|flags=.*runtime"
lipo -archs "$app/Contents/MacOS/SIDwire"

echo "== Notarizing the app and plug-ins"
(cd "$work" && ditto -c -k --keepParent "SIDwire.app" bundles.zip \
    && zip -qry bundles.zip "SIDwire.component" "SIDwire.vst3")
xcrun notarytool submit "$work/bundles.zip" --keychain-profile "$profile" --wait
for bundle in "$app" "$au" "$vst3"; do
    xcrun stapler staple "$bundle"
done

echo "== Making the disk image"
image="$work/image"
mkdir -p "$image/Plug-ins"
ditto "$app" "$image/SIDwire.app"
ditto "$au" "$image/Plug-ins/SIDwire.component"
ditto "$vst3" "$image/Plug-ins/SIDwire.vst3"
ln -s /Applications "$image/Applications"
ln -s /Library/Audio/Plug-Ins/Components "$image/Plug-ins/Audio Unit folder"
ln -s /Library/Audio/Plug-Ins/VST3 "$image/Plug-ins/VST3 folder"
sed "s/@VERSION@/$version/g" tools/dmg-readme.txt > "$image/Read Me.txt"

dmg="$out/SIDwire-$version.dmg"
rm -f "$dmg"
# a writable image first, so the volume can get the app's icon; then compress it
rw="$work/rw.dmg"
hdiutil create -volname "SIDwire $version" -srcfolder "$image" -fs HFS+ -format UDRW "$rw" >/dev/null
mount=$(hdiutil attach -readwrite -noverify -nobrowse "$rw" | tail -1 | awk -F'\t' '{print $NF}')
cp "$app/Contents/Resources/AppIcon.icns" "$mount/.VolumeIcon.icns"
xcrun SetFile -a C "$mount"
hdiutil detach -quiet "$mount"
hdiutil convert "$rw" -format UDZO -o "$dmg" >/dev/null
codesign --sign "$identity" --timestamp "$dmg"

echo "== Notarizing the disk image"
xcrun notarytool submit "$dmg" --keychain-profile "$profile" --wait
xcrun stapler staple "$dmg"

echo "== Checking what a downloaded copy will see"
spctl --assess --type open --context context:primary-signature --verbose=2 "$dmg"
spctl --assess --type execute --verbose=2 "$app"
for bundle in "$au" "$vst3"; do
    xcrun stapler validate "$bundle"
done
xcrun stapler validate "$dmg"

rm -rf "$work"
echo "SHA-256: $(shasum -a 256 "$dmg" | cut -d' ' -f1)"
echo "Done: $dmg"
