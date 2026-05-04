#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-"$ROOT_DIR/build-release"}"
VERSION="${VERSION:-v0.2.0}"
IDENTITY="${SIGN_IDENTITY:-Developer ID Application}"
NOTARY_PROFILE="${NOTARY_PROFILE:-}"

VST3_BUNDLE="$BUILD_DIR/IntelVSTWrapper_artefacts/VST3/Intel VST Wrapper.vst3"
AU_BUNDLE="$BUILD_DIR/IntelVSTWrapper_artefacts/AU/Intel VST Wrapper.component"
ENTITLEMENTS="$ROOT_DIR/Packaging/host.entitlements"
DIST_DIR="$ROOT_DIR/dist"

sign_bundle() {
  local bundle="$1"
  local helper="$bundle/Contents/Resources/IntelVSTBridgeHelper.app"
  local helper_exe="$bundle/Contents/Resources/IntelVSTBridgeHelper"

  if [[ -f "$helper_exe" ]]; then
    codesign --force --timestamp --options runtime \
      --entitlements "$ENTITLEMENTS" \
      --sign "$IDENTITY" \
      "$helper_exe"
  fi

  if [[ -d "$helper" ]]; then
    codesign --force --timestamp --options runtime \
      --entitlements "$ENTITLEMENTS" \
      --sign "$IDENTITY" \
      "$helper"
  fi

  codesign --force --timestamp --options runtime \
    --sign "$IDENTITY" \
    "$bundle"

  codesign --verify --strict --verbose=2 "$bundle"
}

zip_bundle() {
  local bundle="$1"
  local output="$2"

  ditto -c -k --keepParent "$bundle" "$output"
}

notarize_archive() {
  local archive="$1"

  xcrun notarytool submit "$archive" \
    --keychain-profile "$NOTARY_PROFILE" \
    --wait
}

mkdir -p "$DIST_DIR"

if ! security find-identity -v -p codesigning | grep -F "$IDENTITY" >/dev/null; then
  echo "No code-signing identity matching '$IDENTITY' was found." >&2
  echo "Set SIGN_IDENTITY to the exact Developer ID Application identity." >&2
  exit 1
fi

sign_bundle "$VST3_BUNDLE"
sign_bundle "$AU_BUNDLE"

VST3_ZIP="$DIST_DIR/Intel-VST-Wrapper-$VERSION-vst3.zip"
AU_ZIP="$DIST_DIR/Intel-VST-Wrapper-$VERSION-au.zip"

zip_bundle "$VST3_BUNDLE" "$VST3_ZIP"
zip_bundle "$AU_BUNDLE" "$AU_ZIP"

if [[ -n "$NOTARY_PROFILE" ]]; then
  notarize_archive "$VST3_ZIP"
  notarize_archive "$AU_ZIP"
fi

shasum -a 256 "$VST3_ZIP" "$AU_ZIP"
