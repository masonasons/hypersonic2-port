#!/bin/bash
# Builds the plugin for distribution on Mac and packages it as dist/HS2-Port-<version>-Mac.zip
# (VST3, AU, Standalone app and an install note). Run from anywhere: tools/dist/dist-mac.sh
#
# Environment (all optional):
#   HS2_ARCHS           architectures, default "arm64;x86_64" (universal)
#   HS2_SIGN_ID         codesign identity; default: the first "Developer ID Application" identity in the keychain.
#                       Set to "-" for ad-hoc signing (runs only on this Mac without a Gatekeeper prompt).
#   HS2_NOTARY_PROFILE  notarytool keychain profile (xcrun notarytool store-credentials <name> ...). When set,
#                       the zip is notarized and the bundles stapled. Without it, other Macs show a Gatekeeper warning.
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

if [ -n "${HS2_NOTARY_PROFILE:-}" ]; then
    echo "== Notarizing"
    xcrun notarytool submit "$ZIP" --keychain-profile "$HS2_NOTARY_PROFILE" --wait
    for b in "$STAGE"/*.vst3 "$STAGE"/*.component "$STAGE"/*.app; do xcrun stapler staple "$b"; done
    rm -f "$ZIP"
    ditto -c -k --norsrc --noextattr --keepParent "$STAGE" "$ZIP"
else
    echo "HS2_NOTARY_PROFILE not set: not notarized (other Macs will show a Gatekeeper warning)." >&2
fi

echo "== $ZIP"
