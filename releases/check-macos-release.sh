#!/usr/bin/env bash
# Checks a Mac release against what a clean machine needs, and exits non-zero on any failure:
# - every Mach-O is arm64, built for exactly <min-macos>, and has no /Users/ string anywhere in it
# - each bundled dylib has @loader_path as its only rpath
# - every non-system library a binary links is one that ships beside it
# - both Info.plists declare <min-macos>
# - each folder has the licence and notice files
# - libsa3 loads from each place it ships, and exports sa3_get_api
#
# Usage: check-macos-release.sh <dir> <min-macos>
#
# <dir> holds either the staged folders or the final zips, which are extracted to a temporary folder and
# also have their signatures verified.
set -uo pipefail

[ $# = 2 ] || { echo "usage: check-macos-release.sh <dir> <min-macos>" >&2; exit 2; }
DIR="$1"
MIN_MACOS="$2"

FAILED=0
bad() { echo "  FAIL: $*"; FAILED=1; }
LICENCES=(LICENSE-iPlug2.txt LICENSE-VST3-SDK.txt LICENSE-sa3.txt LICENSE-ggml.txt LICENSE-yyjson.txt
          LICENSE-cpp-httplib.txt LICENSE-signalsmith-stretch.txt LICENSE-signalsmith-linear.txt
          THIRD_PARTY_NOTICES.md README.md)

ZIPPED=0
if compgen -G "$DIR/*.zip" >/dev/null; then
  ZIPPED=1
  ROOT="$(mktemp -d)"
  trap 'rm -rf "$ROOT"' EXIT
  for zip in "$DIR"/*.zip; do
    echo "extracting $(basename "$zip")"
    ditto -x -k "$zip" "$ROOT" || bad "could not extract $zip"
  done
else
  ROOT="$DIR"
fi
DEMO="$ROOT/SA3IPlug2Demo-macOS-arm64"
REAPER="$ROOT/SA3ReaperExtension-macOS-arm64"

check_macho() {
  local file="$1" name archs minos leaks rpaths dep base
  name="${file#"$ROOT"/}"
  echo "$name"

  archs="$(lipo -archs "$file" 2>/dev/null)"
  [ "$archs" = arm64 ] || bad "$name is built for: $archs"

  minos="$(otool -l "$file" | awk '/cmd LC_BUILD_VERSION/ { found = 1 } found && $1 == "minos" { print $2; exit }')"
  echo "  minos $minos"
  [ "$minos" = "$MIN_MACOS" ] || bad "$name has minimum macOS '$minos', expected $MIN_MACOS"

  # The whole file rather than strings(1), so load commands and the symbol table are covered too.
  leaks="$(LC_ALL=C grep -a -o '/Users/[[:print:]]*' "$file" | sort -u)"
  [ -z "$leaks" ] || bad "$name holds build-machine paths:"$'\n'"$leaks"

  rpaths="$(otool -l "$file" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }' | tr '\n' ' ' | sed 's/ $//')"
  echo "  rpaths [${rpaths}]"
  case "$(basename "$file")" in
    reaper_*) [ -z "$rpaths" ] || bad "$name has rpaths: $rpaths" ;;
    *.dylib) [ "$rpaths" = "@loader_path" ] || bad "$name has rpaths other than @loader_path: $rpaths" ;;
    *) [ -z "$rpaths" ] || bad "$name has rpaths: $rpaths" ;;
  esac

  # otool -L lists a dylib's own id first, which resolves to itself.
  while IFS= read -r dep; do
    case "$dep" in
      /usr/lib/*|/System/Library/*) continue ;;
      @rpath/*|@loader_path/*) base="${dep##*/}" ;;
      *) bad "$name links outside the bundle: $dep"; continue ;;
    esac
    [ -f "$(dirname "$file")/$base" ] || bad "$name links $dep, which does not ship beside it"
  done < <(otool -L "$file" | awk 'NR > 1 { print $1 }')
}

check_plist() {
  local value
  value="$(plutil -extract LSMinimumSystemVersion raw "$1/Contents/Info.plist")"
  echo "${1#"$ROOT"/} LSMinimumSystemVersion $value"
  [ "$value" = "$MIN_MACOS" ] || bad "$1 declares LSMinimumSystemVersion $value, expected $MIN_MACOS"
}

check_folder() {
  local licence
  for licence in "${LICENCES[@]}"; do
    [ -s "$1/$licence" ] || bad "${1#"$ROOT"/} has no $licence"
  done
}

check_loads() {
  echo "loading $1"
  python3 - "$1" <<'PY' || bad "libsa3 did not load from $1"
import ctypes, sys
ctypes.CDLL(sys.argv[1]).sa3_get_api
PY
}

for bundle in "$DEMO/SA3IPlug2Demo.vst3" "$DEMO/SA3IPlug2Demo.app"; do
  [ -d "$bundle" ] || { bad "no $bundle"; continue; }
  check_plist "$bundle"
  for file in "$bundle"/Contents/MacOS/*; do check_macho "$file"; done
  check_loads "$bundle/Contents/MacOS/libsa3.dylib"
  if [ "$ZIPPED" = 1 ]; then
    codesign --verify --deep --strict "$bundle" || bad "$bundle fails signature verification"
  fi
done

if [ -f "$REAPER/reaper_SA3ReaperExtension.dylib" ]; then
  check_macho "$REAPER/reaper_SA3ReaperExtension.dylib"
  for file in "$REAPER"/SA3ReaperExtension/*.dylib; do
    check_macho "$file"
    if [ "$ZIPPED" = 1 ]; then
      codesign --verify --strict "$file" || bad "$file fails signature verification"
    fi
  done
  check_loads "$REAPER/SA3ReaperExtension/libsa3.dylib"
else
  bad "no REAPER extension in $REAPER"
fi

check_folder "$DEMO"
check_folder "$REAPER"

if [ "$FAILED" != 0 ]; then
  echo "macOS release checks FAILED"
  exit 1
fi
echo "macOS release checks passed: minimum macOS $MIN_MACOS, @loader_path only, no /Users/ strings"
