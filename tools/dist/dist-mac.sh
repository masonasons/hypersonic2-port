#!/bin/bash
# Builds the plugin for distribution on Mac and packages it as dist/HS2-Port-<version>-Mac.zip
# (VST3, AU, Standalone app and an install note). Run from anywhere: tools/dist/dist-mac.sh
#
# Environment (all optional):
#   HS2_ARCHS           architectures, default "arm64;x86_64" (universal)
#   HS2_SIGN_ID         codesign identity; default: the first "Developer ID Application" identity in the keychain.
#                       Set to "-" for ad-hoc signing (runs only on this Mac without a Gatekeeper prompt).
#   Notarization, either of (without one, other Macs show a Gatekeeper warning):
#   ASC_KEY_ID, ASC_ISSUER_ID  an App Store Connect API key; the .p8 is read from ASC_KEY_PATH, default
#                       ~/.appstoreconnect/private_keys/AuthKey_<ASC_KEY_ID>.p8
#   HS2_NOTARY_PROFILE  a notarytool keychain profile (xcrun notarytool store-credentials <name> ...)
#   When notarizing, the zip is submitted, and the bundles are stapled and zipped again.
#
# The installer, dist/HS2-Port-<version>-Mac.pkg (VST3, AU and the app, installed for all users, each one optional),
# is built when a "Developer ID Installer" identity is in the keychain (a different certificate from the Application
# one) and notarized like the zip:
#   HS2_INSTALLER_SIGN_ID  its name; default: the first "Developer ID Installer" identity in the keychain
#   HS2_REQUIRE_PKG=1      stop when there is none (CI release builds), instead of skipping the installer
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="$(sed -n 's/^ *VERSION \([0-9][0-9.]*\).*/\1/p' "$ROOT/plugin/CMakeLists.txt" | head -1)"
ARCHS="${HS2_ARCHS:-arm64;x86_64}"
BUILD="$ROOT/build_dist_mac"
NAME="HS2-Port-$VERSION-Mac"
STAGE="$ROOT/dist/$NAME"
ZIP="$ROOT/dist/$NAME.zip"
ART="$BUILD/plugin/HS2Port_artefacts/Release"

SIGN_ID="${HS2_SIGN_ID:-}"
if [ -z "$SIGN_ID" ]; then
    SIGN_ID="$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application:[^"]*\)".*/\1/p' | head -1)"
fi
if [ -z "$SIGN_ID" ]; then
    echo "No Developer ID Application identity found; signing ad-hoc." >&2
    SIGN_ID="-"
fi

echo "== HS2 Port $VERSION for Mac ($ARCHS), signing with: $SIGN_ID"

# The oldest macOS current Xcode can target (Xcode 27: 12.0).
cmake -S "$ROOT" -B "$BUILD" -G Xcode -DHS2_BUILD_PLUGIN=ON \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS" -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
cmake --build "$BUILD" --config Release --target HS2Port_VST3 HS2Port_AU HS2Port_Standalone -- -quiet

rm -rf "$STAGE" "$ZIP"
mkdir -p "$STAGE"
ditto "$ART/VST3/HS2 Port.vst3" "$STAGE/HS2 Port.vst3"
ditto "$ART/AU/HS2 Port.component" "$STAGE/HS2 Port.component"
ditto "$ART/Standalone/HS2 Port.app" "$STAGE/HS2 Port.app"

# Hardened runtime (needed for notarization). The Standalone app records from the audio input, so it needs the
# audio-input entitlement; the plugins run under their host's entitlements.
ENT="$BUILD/standalone.entitlements"
cat > "$ENT" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.device.audio-input</key><true/></dict></plist>
EOF
SIGN_OPTS=(--force --options runtime)
[ "$SIGN_ID" != "-" ] && SIGN_OPTS+=(--timestamp)
codesign "${SIGN_OPTS[@]}" --sign "$SIGN_ID" "$STAGE/HS2 Port.vst3"
codesign "${SIGN_OPTS[@]}" --sign "$SIGN_ID" "$STAGE/HS2 Port.component"
codesign "${SIGN_OPTS[@]}" --entitlements "$ENT" --sign "$SIGN_ID" "$STAGE/HS2 Port.app"
for b in "$STAGE"/*.vst3 "$STAGE"/*.component "$STAGE"/*.app; do
    codesign --verify --strict "$b"
    echo "   $(basename "$b"): $(lipo -archs "$b/Contents/MacOS/HS2 Port")"
done

cat > "$STAGE/Install.txt" <<EOF
HS2 Port $VERSION for Mac (macOS 12 or later, Apple Silicon and Intel)

Copy the plugins into your user plug-in folders (Finder: Go > Go to Folder):
  HS2 Port.vst3       ->  ~/Library/Audio/Plug-Ins/VST3
  HS2 Port.component  ->  ~/Library/Audio/Plug-Ins/Components
  HS2 Port.app        ->  /Applications (the standalone version, optional)

The plugin reads your own Hypersonic 2 content. Put the content folder at ~/Hypersonic 2 Content,
or choose its location with "Choose content folder..." in the plugin's Settings.
EOF

ditto -c -k --norsrc --noextattr --keepParent "$STAGE" "$ZIP"

NOTARY_AUTH=()
if [ -n "${ASC_KEY_ID:-}" ] && [ -n "${ASC_ISSUER_ID:-}" ]; then
    KEY_PATH="${ASC_KEY_PATH:-$HOME/.appstoreconnect/private_keys/AuthKey_${ASC_KEY_ID}.p8}"
    [ -f "$KEY_PATH" ] || { echo "The App Store Connect key is not at $KEY_PATH" >&2; exit 1; }
    NOTARY_AUTH=(--key "$KEY_PATH" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER_ID")
elif [ -n "${HS2_NOTARY_PROFILE:-}" ]; then
    NOTARY_AUTH=(--keychain-profile "$HS2_NOTARY_PROFILE")
fi

# Submits a file for notarization and waits; stops the script when Apple does not accept it.
notarize() {
    echo "== Notarizing $(basename "$1") (this can take a few minutes)"
    local result id
    result="$(xcrun notarytool submit "$1" "${NOTARY_AUTH[@]}" --wait --output-format json)"
    echo "$result"
    if ! echo "$result" | grep -q '"status" *: *"Accepted"'; then
        id="$(echo "$result" | sed -n 's/.*"id" *: *"\([^"]*\)".*/\1/p' | head -1)"
        [ -n "$id" ] && xcrun notarytool log "$id" "${NOTARY_AUTH[@]}" >&2
        echo "Notarization was not accepted." >&2
        exit 1
    fi
}

if [ ${#NOTARY_AUTH[@]} -gt 0 ]; then
    [ "$SIGN_ID" = "-" ] && { echo "Ad-hoc signed bundles cannot be notarized." >&2; exit 1; }
    notarize "$ZIP"
    for b in "$STAGE"/*.vst3 "$STAGE"/*.component "$STAGE"/*.app; do
        xcrun stapler staple "$b"
        xcrun stapler validate "$b"
    done
    rm -f "$ZIP"
    ditto -c -k --norsrc --noextattr --keepParent "$STAGE" "$ZIP"
else
    echo "No notarization credentials: not notarized (other Macs will show a Gatekeeper warning)." >&2
fi
echo "== $ZIP"

# ---- The installer: one component package per bundle (not relocatable, so an app moved elsewhere is not updated in
# place), combined by productbuild into a package whose Customize page lets each one be left out.
INSTALLER_ID="${HS2_INSTALLER_SIGN_ID:-}"
if [ -z "$INSTALLER_ID" ]; then
    INSTALLER_ID="$(security find-identity -v | sed -n 's/.*"\(Developer ID Installer:[^"]*\)".*/\1/p' | head -1)"
fi
if [ -z "$INSTALLER_ID" ]; then
    if [ "${HS2_REQUIRE_PKG:-0}" = 1 ]; then
        echo "No Developer ID Installer identity: the installer package cannot be signed." >&2
        exit 1
    fi
    echo "No Developer ID Installer identity: no installer package (only the zip)." >&2
    exit 0
fi

PKG="$ROOT/dist/$NAME.pkg"
WORK="$BUILD/pkg"
rm -rf "$WORK" "$PKG"
mkdir -p "$WORK/resources"
component() {  # id, bundle, install location
    local root="$WORK/root-$1" plist="$WORK/$1.plist"
    mkdir -p "$root"
    ditto "$STAGE/$2" "$root/$2"
    pkgbuild --analyze --root "$root" "$plist"
    /usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" "$plist"
    pkgbuild --root "$root" --component-plist "$plist" --identifier "com.hs2port.HS2Port.$1" --version "$VERSION" \
        --install-location "$3" "$WORK/$1.pkg"
}
component vst3 "HS2 Port.vst3" "/Library/Audio/Plug-Ins/VST3"
component au "HS2 Port.component" "/Library/Audio/Plug-Ins/Components"
component app "HS2 Port.app" "/Applications"

cat > "$WORK/resources/welcome.txt" <<EOF
HS2 Port $VERSION for Mac (macOS 12 or later, Apple Silicon and Intel)

This installs, for all users of this Mac:
  - the VST3 plug-in, in /Library/Audio/Plug-Ins/VST3
  - the Audio Unit, in /Library/Audio/Plug-Ins/Components
  - the standalone app, in /Applications
Choose Customize to leave any of them out.

HS2 Port plays your own Hypersonic 2 content. Put the content folder at ~/Hypersonic 2 Content, or choose its location
with "Choose content folder..." in the plugin's Settings.

If you copied an earlier version into your own Library (~/Library/Audio/Plug-Ins), remove that copy, so your DAW does
not find two.
EOF

cat > "$WORK/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>HS2 Port $VERSION</title>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <volume-check>
        <allowed-os-versions><os-version min="12.0"/></allowed-os-versions>
    </volume-check>
    <choices-outline>
        <line choice="vst3"/>
        <line choice="au"/>
        <line choice="app"/>
    </choices-outline>
    <choice id="vst3" title="VST3 plug-in" description="HS2 Port as a VST3 plug-in, in /Library/Audio/Plug-Ins/VST3.">
        <pkg-ref id="com.hs2port.HS2Port.vst3"/>
    </choice>
    <choice id="au" title="Audio Unit" description="HS2 Port as an Audio Unit (Logic, GarageBand and others), in /Library/Audio/Plug-Ins/Components.">
        <pkg-ref id="com.hs2port.HS2Port.au"/>
    </choice>
    <choice id="app" title="Standalone app" description="HS2 Port as a standalone app, in /Applications.">
        <pkg-ref id="com.hs2port.HS2Port.app"/>
    </choice>
    <pkg-ref id="com.hs2port.HS2Port.vst3" version="$VERSION" onConclusion="none">vst3.pkg</pkg-ref>
    <pkg-ref id="com.hs2port.HS2Port.au" version="$VERSION" onConclusion="none">au.pkg</pkg-ref>
    <pkg-ref id="com.hs2port.HS2Port.app" version="$VERSION" onConclusion="none">app.pkg</pkg-ref>
</installer-gui-script>
EOF

echo "== Installer package, signing with: $INSTALLER_ID"
productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" --resources "$WORK/resources" \
    --sign "$INSTALLER_ID" --timestamp "$PKG"
pkgutil --check-signature "$PKG"
if [ ${#NOTARY_AUTH[@]} -gt 0 ]; then
    notarize "$PKG"
    xcrun stapler staple "$PKG"
    xcrun stapler validate "$PKG"
fi
echo "== $PKG"
