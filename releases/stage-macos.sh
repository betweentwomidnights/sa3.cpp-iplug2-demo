#!/usr/bin/env bash
# Lays out the folders sign-and-notarize.sh takes, from a finished Mac release build, with the same
# licence and notice files the Windows zips carry.
#
# Usage: stage-macos.sh <demo-build-dir> <sa3.cpp-dir> <stage-dir>
#
# <stage-dir> is cleared first. Run it from anywhere: this checkout supplies the demo's own files.
set -euo pipefail

fail() { echo "stage-macos: $*" >&2; exit 1; }

[ $# = 3 ] || fail "usage: stage-macos.sh <demo-build-dir> <sa3.cpp-dir> <stage-dir>"
OUT_DIR="$(cd "$1" && pwd)/out"
SA3="$(cd "$2" && pwd)"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$3"
IPLUG2="$ROOT/vendor/iPlug2"

DEMO="$STAGE/SA3IPlug2Demo-macOS-arm64"
REAPER="$STAGE/SA3ReaperExtension-macOS-arm64"

[ -d "$OUT_DIR/SA3ReaperExtension" ] || fail "no REAPER runtime folder in $OUT_DIR"

rm -rf "$DEMO" "$REAPER"
mkdir -p "$DEMO" "$REAPER"

# ditto, because a bundle holds symlinks and signatures that cp -R can disturb.
ditto "$OUT_DIR/SA3IPlug2Demo.vst3" "$DEMO/SA3IPlug2Demo.vst3"
ditto "$OUT_DIR/SA3IPlug2Demo.app" "$DEMO/SA3IPlug2Demo.app"
cp "$OUT_DIR/reaper_SA3ReaperExtension.dylib" "$REAPER/"
ditto "$OUT_DIR/SA3ReaperExtension" "$REAPER/SA3ReaperExtension"

for folder in "$DEMO" "$REAPER"; do
  cp "$IPLUG2/LICENSE.txt" "$folder/LICENSE-iPlug2.txt"
  cp "$IPLUG2/Dependencies/IPlug/VST3_SDK/LICENSE.txt" "$folder/LICENSE-VST3-SDK.txt"
  cp "$SA3/LICENSE" "$folder/LICENSE-sa3.txt"
  cp "$SA3/ggml/LICENSE" "$folder/LICENSE-ggml.txt"
  cp "$SA3/vendor/yyjson/LICENSE" "$folder/LICENSE-yyjson.txt"
  cp "$SA3/vendor/cpp-httplib/LICENSE" "$folder/LICENSE-cpp-httplib.txt"
  cp "$SA3/vendor/signalsmith-stretch/LICENSE.txt" "$folder/LICENSE-signalsmith-stretch.txt"
  cp "$SA3/vendor/signalsmith-linear/LICENSE.txt" "$folder/LICENSE-signalsmith-linear.txt"
  cp "$SA3/docs/THIRD_PARTY_NOTICES.md" "$folder/THIRD_PARTY_NOTICES.md"
  cp "$ROOT/README.md" "$folder/README.md"
done

echo "staged -> $STAGE"
