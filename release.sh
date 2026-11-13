#!/bin/bash
# Builds the public release: "dist/Gizzy's Gaming Hub.app" with the engine inside, and "dist/GizzysGamingHub-<version>.dmg".
# The app is ad-hoc signed (no paid Apple Developer account): the first launch needs right-click › Open (see README).
# Nothing personal goes in: no keys, no games, no saves, no work folders, no developer certificate.
set -euo pipefail
cd "$(dirname "$0")"
VERSION=$(cat VERSION)
./build.sh
SRC="build/Gizzy's Gaming Hub.app"
rm -rf dist && mkdir -p dist
APP="dist/Gizzy's Gaming Hub.app"
ditto "$SRC" "$APP"

echo "==> Bundling the engine"
ENG="$APP/Contents/Resources/Engine/nxnative"
mkdir -p "$ENG"
rsync -a --delete \
  --exclude '.git' --exclude '__pycache__' --exclude '*.pyc' --exclude '.DS_Store' \
  --exclude 'dm/host/gen/' --exclude 'obj/' --exclude 'bin/Debug/' --exclude 'bin/Release/' --exclude 'dm/host/dmhost' --exclude 'dm/host/dmhost.dSYM' --exclude '*.o' --exclude '*.a' \
  Engines/nxnative/ "$ENG/"
echo "$VERSION-$(date +%Y%m%d%H%M)" > "$ENG/.gizzy-engine-version"

echo "==> Checking for anything that must not ship"
BAD=$(find "$APP" \( -iname 'prod.keys' -o -iname 'title.keys' -o -iname '*.nsp' -o -iname '*.xci' -o -iname '*.nsz' -o -iname '*.nup' -o -iname '*.nca' \) -print)
if [ -n "$BAD" ]; then echo "refusing to ship:"; echo "$BAD"; exit 1; fi

echo "==> Signing (ad-hoc)"
codesign --force --deep --sign - --entitlements Resources/MikesGameRoom.entitlements "$APP"
codesign --verify --deep "$APP" && echo "   signature ok"

echo "==> Building the DMG"
STAGE=$(mktemp -d)
ditto "$APP" "$STAGE/Gizzy's Gaming Hub.app"
ln -s /Applications "$STAGE/Applications"
cp "Resources/Release/READ ME FIRST.txt" "$STAGE/"
DMG="dist/GizzysGamingHub-$VERSION.dmg"
hdiutil create -volname "Gizzy's Gaming Hub" -srcfolder "$STAGE" -ov -format UDZO -fs HFS+ "$DMG" >/dev/null
rm -rf "$STAGE"
shasum -a 256 "$DMG" | tee "$DMG.sha256"
ls -lh "$DMG"
echo "==> Done: $DMG"
