#!/bin/bash
# Gizzy's Gaming Hub first-run setup: installs what the native engine needs to prepare games.
# Run by the hub's setup window with ENGINE_SRC (the engine inside the app), ENGINE_DST, TOOLS and XCODE set.
# Prints "STEP <n> <title>", then "OK <n>" or "FAIL <n> <reason>" for each step; anything else is detail for the log.
set -u
export PATH="$TOOLS/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
mkdir -p "$TOOLS/bin"
fail() { echo "FAIL $1 $2"; exit 1; }

echo "STEP 1 Checking this Mac"
[ "$(uname -m)" = "arm64" ] || fail 1 "an Apple silicon Mac (M1 or later) is required: games run natively on the Apple chip"
echo "macOS $(sw_vers -productVersion)"
echo "OK 1"

echo "STEP 2 Checking Xcode and the Metal compiler"
[ -x "$XCODE/Contents/Developer/usr/bin/xcodebuild" ] || fail 2 "Xcode was not found at $XCODE: install Xcode from the App Store, open it once, then run setup again"
export DEVELOPER_DIR="$XCODE/Contents/Developer"
if ! xcrun -sdk macosx metal --version >/dev/null 2>&1; then
  echo "downloading the Metal toolchain (Xcode component, about 700 MB)…"
  xcodebuild -downloadComponent MetalToolchain || fail 2 "the Metal toolchain could not be installed: open Xcode › Settings › Components and install Metal Toolchain"
fi
xcrun -sdk macosx metal --version 2>&1 | head -1
echo "OK 2"

echo "STEP 3 Installing the native engine"
if [ -n "${ENGINE_SRC:-}" ] && [ -d "$ENGINE_SRC" ] && [ "$ENGINE_SRC" != "$ENGINE_DST" ]; then
  stamp_src=$(cat "$ENGINE_SRC/.gizzy-engine-version" 2>/dev/null || echo none)
  stamp_dst=$(cat "$ENGINE_DST/.gizzy-engine-version" 2>/dev/null || echo missing)
  if [ "$stamp_src" != "$stamp_dst" ]; then
    mkdir -p "$(dirname "$ENGINE_DST")"
    tmp="$ENGINE_DST.new"; rm -rf "$tmp"
    ditto "$ENGINE_SRC" "$tmp" || fail 3 "copying the engine failed"
    [ -d "$ENGINE_DST" ] && mv "$ENGINE_DST" "$ENGINE_DST.old.$$" && rm -rf "$ENGINE_DST.old.$$"
    mv "$tmp" "$ENGINE_DST" || fail 3 "installing the engine failed"
    echo "engine $stamp_src installed"
  else echo "engine $stamp_dst already installed"; fi
fi
[ -f "$ENGINE_DST/scripts/build_ipa_from_nsp.py" ] || fail 3 "the engine is missing from $ENGINE_DST"
echo "OK 3"

echo "STEP 4 Installing zstd and glslang (Homebrew)"
if ! command -v zstd >/dev/null || ! command -v glslangValidator >/dev/null; then
  command -v brew >/dev/null || fail 4 "Homebrew is needed for two small tools (zstd, glslang): install it from https://brew.sh, then run setup again"
  brew install zstd glslang || fail 4 "brew install zstd glslang failed"
fi
echo "OK 4"

echo "STEP 5 Setting up Python and the engine's packages"
if [ ! -x "$TOOLS/bin/uv" ]; then
  echo "downloading uv (Python manager)…"
  curl -fsSL https://github.com/astral-sh/uv/releases/latest/download/uv-aarch64-apple-darwin.tar.gz | tar -xz -C "$TOOLS/bin" --strip-components 1 || fail 5 "downloading uv failed (check the internet connection)"
fi
export UV_PYTHON_INSTALL_DIR="$TOOLS/python" UV_CACHE_DIR="$TOOLS/uv-cache"
[ -x "$TOOLS/venv/bin/python3" ] || uv venv --python 3.14 "$TOOLS/venv" || fail 5 "creating the Python environment failed"
uv pip install --python "$TOOLS/venv/bin/python3" lz4 "capstone>=5,<6" fonttools zstandard nsz || fail 5 "installing the Python packages failed"
"$TOOLS/venv/bin/python3" --version
echo "OK 5"

echo "STEP 6 Installing .NET (shader translator)"
if [ ! -x "$TOOLS/dotnet/dotnet" ]; then
  curl -fsSL https://dot.net/v1/dotnet-install.sh -o "$TOOLS/dotnet-install.sh" || fail 6 "downloading the .NET installer failed"
  bash "$TOOLS/dotnet-install.sh" --channel 10.0 --install-dir "$TOOLS/dotnet" --no-path || fail 6 ".NET install failed"
fi
"$TOOLS/dotnet/dotnet" --version
echo "OK 6"
echo "DONE"
