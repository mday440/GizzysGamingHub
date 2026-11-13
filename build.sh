#!/bin/bash
# Builds "build/Gizzy's Gaming Hub.app".  Usage:  ./build.sh            (build)
#                                          ./build.sh --install  (build + copy to /Applications)
#                                          ./build.sh --open     (build + launch)
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v swift >/dev/null; then
  echo "Swift not found. Install Xcode or the Command Line Tools:  xcode-select --install" >&2
  exit 1
fi

echo "==> Building (release)"
swift build -c release --product MikesGameRoom
swift build -c release --product mgrcli
swift build -c release --product mgrzbic
BIN_DIR="$(swift build -c release --show-bin-path)"

APP="build/Gizzy's Gaming Hub.app"
# the app used to be built as build/MikesGameRoom.app: set it aside (one bundle per ID keeps Launch Services clear)
if [ -d build/MikesGameRoom.app ]; then mkdir -p build/_to_delete && mv build/MikesGameRoom.app "build/_to_delete/MikesGameRoom-$(date +%s).app"; fi
echo "==> Assembling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN_DIR/MikesGameRoom" "$APP/Contents/MacOS/MikesGameRoom"
cp Resources/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
cp Resources/GizzyLogo.png "$APP/Contents/Resources/GizzyLogo.png"   # the wordmark in the app's corner
cp Resources/IntroEmblem.png Resources/IntroLetters.png Resources/IntroSound.m4a "$APP/Contents/Resources/"   # launch intro
mkdir -p "$APP/Contents/Resources/Fonts" && cp Resources/Fonts/* "$APP/Contents/Resources/Fonts/"   # Unbounded, Manrope (OFL)
mkdir -p "$APP/Contents/Resources/Setup" && cp Resources/Setup/setup.sh "$APP/Contents/Resources/Setup/"   # first-run setup (EngineSetup.swift)
cp "$BIN_DIR/mgrzbic" "$APP/Contents/MacOS/mgrzbic"   # the engine's ZBIC decoder travels with the app
cp "$BIN_DIR/mgrcli" build/mgrcli
mkdir -p .build && cp "$BIN_DIR/mgrzbic" build/mgrzbic && cp "$BIN_DIR/mgrzbic" .build/mgrzbic   # used by Engines/nxnative/dm/tools/nso.py
# the hypervisor entitlement lets game code run natively (ad-hoc signature is enough on your own Mac)
codesign --force --sign - --entitlements Resources/MikesGameRoom.entitlements build/mgrcli >/dev/null 2>&1 || true

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Gizzy's Gaming Hub</string>
  <key>CFBundleDisplayName</key><string>Gizzy's Gaming Hub</string>
  <key>CFBundleIdentifier</key><string>com.mikesgameroom.app</string>
  <key>CFBundleExecutable</key><string>MikesGameRoom</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$(cat VERSION 2>/dev/null || echo 0.1.0)</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>Gizzy's Gaming Hub</string>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>Switch Game</string>
      <key>CFBundleTypeRole</key><string>Viewer</string>
      <key>LSHandlerRank</key><string>Alternate</string>
      <key>CFBundleTypeExtensions</key>
      <array><string>nup</string><string>nsp</string><string>xci</string><string>nca</string><string>nso</string><string>nro</string><string>nsz</string><string>xcz</string></array>
    </dict>
  </array>
</dict>
</plist>
PLIST

# A fixed personal certificate (made once by .mgr/mkcert.sh) keeps the app "the same app" to the Keychain across rebuilds, so it stops asking for the password; ad-hoc without it
SIGN_ID="Gizzys Gaming Hub Signing"
if security find-certificate -c "$SIGN_ID" >/dev/null 2>&1 && codesign --force --deep --sign "$SIGN_ID" --entitlements Resources/MikesGameRoom.entitlements "$APP" >/dev/null 2>&1; then
    echo "==> Signing ($SIGN_ID)"
else
    echo "==> Signing (ad-hoc)"
    codesign --force --deep --sign - --entitlements Resources/MikesGameRoom.entitlements "$APP" >/dev/null 2>&1 || echo "   (codesign skipped)"
fi

touch "$APP"   # so Finder and the Dock pick up a new icon
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP" >/dev/null 2>&1 || true
echo "==> Done: $APP"
case "${1:-}" in
  --install) rm -rf "/Applications/Gizzy's Gaming Hub.app"; cp -R "$APP" /Applications/ && echo "Installed to /Applications"; open "/Applications/Gizzy's Gaming Hub.app" ;;
  --open)    open "$APP" ;;
esac
