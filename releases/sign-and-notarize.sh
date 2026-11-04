#!/usr/bin/env bash
# Signs, notarizes, staples, zips and hashes the demo's Mac release folders, following
# MACOS_SIGNING.md. Credentials come from the keychain or the environment, so nothing secret belongs
# in this file.
#
# Usage: sign-and-notarize.sh <notary-profile> <stage-dir> [signing-identity]
#        sign-and-notarize.sh --api-key <stage-dir> [signing-identity]
#        sign-and-notarize.sh --ad-hoc <stage-dir>
#
# --api-key logs in to the notary service with SA3_NOTARY_KEY (a .p8 path), SA3_NOTARY_KEY_ID and
# SA3_NOTARY_ISSUER rather than a keychain profile, for CI. --ad-hoc signs without an identity and skips
# notarizing, which is what a dry run without the secrets gets: its zips are named -unsigned, run only on
# the Mac you put them on, and must never be published.
#
# <stage-dir> holds:
#   SA3IPlug2Demo-macOS-arm64/SA3IPlug2Demo.vst3
#   SA3IPlug2Demo-macOS-arm64/SA3IPlug2Demo.app
#   SA3ReaperExtension-macOS-arm64/reaper_SA3ReaperExtension.dylib
#   SA3ReaperExtension-macOS-arm64/SA3ReaperExtension/*.dylib
#
# The final zips and SHA256SUMS-macos land in <stage-dir>/signed. Pass [signing-identity] only when
# the keychain has more than one Developer ID Application identity.
set -euo pipefail

fail() { echo "sign-and-notarize: $*" >&2; exit 1; }

[ $# -ge 2 ] || fail "usage: sign-and-notarize.sh <notary-profile|--api-key|--ad-hoc> <stage-dir> [signing-identity]"
PROFILE="$1"
STAGE="$(cd "$2" && pwd)"
IDENTITY="${3:-}"
OUT="$STAGE/signed"
DEMO="$STAGE/SA3IPlug2Demo-macOS-arm64"
REAPER="$STAGE/SA3ReaperExtension-macOS-arm64"

[ -d "$DEMO/SA3IPlug2Demo.vst3" ] && [ -d "$DEMO/SA3IPlug2Demo.app" ] || fail "no demo bundles in $DEMO"
[ -f "$REAPER/reaper_SA3ReaperExtension.dylib" ] || fail "no REAPER extension in $REAPER"

case "$PROFILE" in
  --ad-hoc)
    IDENTITY="-"
    ZIP_SUFFIX="-unsigned"
    ;;
  --api-key)
    : "${SA3_NOTARY_KEY:?}" "${SA3_NOTARY_KEY_ID:?}" "${SA3_NOTARY_ISSUER:?}"
    [ -f "$SA3_NOTARY_KEY" ] || fail "SA3_NOTARY_KEY $SA3_NOTARY_KEY does not exist"
    NOTARY_AUTH=(--key "$SA3_NOTARY_KEY" --key-id "$SA3_NOTARY_KEY_ID" --issuer "$SA3_NOTARY_ISSUER")
    ZIP_SUFFIX=""
    ;;
  *)
    NOTARY_AUTH=(--keychain-profile "$PROFILE")
    ZIP_SUFFIX=""
    ;;
esac

if [ -z "$IDENTITY" ]; then
  identities="$(security find-identity -v -p codesigning | grep "Developer ID Application" || true)"
  [ -n "$identities" ] || fail "no Developer ID Application identity in the keychain"
  [ "$(printf '%s\n' "$identities" | wc -l | tr -d ' ')" = 1 ] ||
    fail "several identities found, so pass one as the third argument:"$'\n'"$identities"
  IDENTITY="$(printf '%s\n' "$identities" | awk '{print $2}')"
fi
if [ "$PROFILE" != --ad-hoc ]; then
  xcrun notarytool history "${NOTARY_AUTH[@]}" >/dev/null 2>&1 ||
    fail "notary login '$PROFILE' does not work"
fi

VERSION="$(plutil -extract CFBundleShortVersionString raw "$DEMO/SA3IPlug2Demo.vst3/Contents/Info.plist")"
echo "version $VERSION, notary login $PROFILE"

require_loader_path_only() {
  local rpaths
  rpaths="$(otool -l "$1" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }')"
  [ "$rpaths" = "@loader_path" ] || fail "$1 has rpaths other than @loader_path: $rpaths"
}

# An ad-hoc identity has no team, so the hardened runtime's library validation would refuse the bundled
# dylibs, and there is no timestamp to get.
sign() {
  if [ "$IDENTITY" = "-" ]; then
    codesign --force --sign - "$1"
  else
    codesign --force --options runtime --timestamp --sign "$IDENTITY" "$1"
  fi
}

sign_bundle() {
  local lib
  for lib in "$1"/Contents/MacOS/*.dylib; do
    require_loader_path_only "$lib"
    sign "$lib"
  done
  sign "$1"
  codesign --verify --deep --strict --verbose=2 "$1"
}

zip_folder() { ditto -c -k --norsrc --keepParent "$1" "$2"; }

notarize() {
  local result status id
  echo "notarizing $(basename "$1")"
  result="$(xcrun notarytool submit "$1" "${NOTARY_AUTH[@]}" --wait --timeout 30m --output-format json)" || true
  status="$(printf '%s' "$result" | plutil -extract status raw -o - - 2>/dev/null || true)"
  id="$(printf '%s' "$result" | plutil -extract id raw -o - - 2>/dev/null || true)"
  echo "notarization $id: ${status:-no status}"
  if [ "$status" != Accepted ]; then
    echo "$result" >&2
    [ -z "$id" ] || xcrun notarytool log "$id" "${NOTARY_AUTH[@]}" >&2 || true
    fail "$(basename "$1") was not accepted"
  fi
}

# A ticket can take a moment to become available after Accepted.
with_retries() {
  local attempt
  for attempt in 1 2 3 4 5; do
    "$@" && return 0
    [ "$attempt" = 5 ] || { echo "retrying in 30s: $*"; sleep 30; }
  done
  return 1
}

rm -rf "$OUT"
mkdir -p "$OUT"

sign_bundle "$DEMO/SA3IPlug2Demo.vst3"
sign_bundle "$DEMO/SA3IPlug2Demo.app"
DEMO_ZIP="$OUT/SA3IPlug2Demo-$VERSION-macos-metal-arm64$ZIP_SUFFIX.zip"
zip_folder "$DEMO" "$DEMO_ZIP"

for lib in "$REAPER"/SA3ReaperExtension/*.dylib; do
  require_loader_path_only "$lib"
  sign "$lib"
  codesign --verify --strict --verbose=2 "$lib"
done
sign "$REAPER/reaper_SA3ReaperExtension.dylib"
codesign --verify --strict --verbose=2 "$REAPER/reaper_SA3ReaperExtension.dylib"
REAPER_ZIP="$OUT/SA3ReaperExtension-$VERSION-macos-metal-arm64$ZIP_SUFFIX.zip"
zip_folder "$REAPER" "$REAPER_ZIP"

if [ "$PROFILE" = --ad-hoc ]; then
  ( cd "$OUT" && shasum -a 256 ./*.zip | sed 's# \./# #' > SHA256SUMS-macos )
  cat "$OUT/SHA256SUMS-macos"
  echo "ad-hoc archives, not notarized -> $OUT"
  exit 0
fi

notarize "$DEMO_ZIP"
notarize "$REAPER_ZIP"

# A zip can't hold a stapled ticket of its own, so staple the bundles and zip them again. The REAPER
# extension is a bare dylib, which can't be stapled, so its first zip is final.
with_retries xcrun stapler staple "$DEMO/SA3IPlug2Demo.vst3"
with_retries xcrun stapler staple "$DEMO/SA3IPlug2Demo.app"
rm "$DEMO_ZIP"
zip_folder "$DEMO" "$DEMO_ZIP"

( cd "$OUT" && shasum -a 256 ./*.zip | sed 's# \./# #' > SHA256SUMS-macos )

CHECK="$(mktemp -d)"
trap 'rm -rf "$CHECK"' EXIT
ditto -x -k "$DEMO_ZIP" "$CHECK"
ditto -x -k "$REAPER_ZIP" "$CHECK"
spctl -a -vv -t exec "$CHECK/SA3IPlug2Demo-macOS-arm64/SA3IPlug2Demo.app"
xcrun stapler validate "$CHECK/SA3IPlug2Demo-macOS-arm64/SA3IPlug2Demo.app"
xcrun stapler validate "$CHECK/SA3IPlug2Demo-macOS-arm64/SA3IPlug2Demo.vst3"
with_retries codesign --verify --check-notarization -R=notarized \
  "$CHECK/SA3ReaperExtension-macOS-arm64/reaper_SA3ReaperExtension.dylib"

cat "$OUT/SHA256SUMS-macos"
echo "signed archives -> $OUT"
