#!/usr/bin/env bash
# Package the native (JUCE, no Electron) Stemcel for macOS: a Release build of the
# StemcelNative app bundle, ad-hoc signed, smoke-tested from inside the bundle, then
# shipped as a DMG and a zipped .app in <repo>/dist/.
#
# The bundle carries everything beside the executable (Contents/MacOS): LAME, the MIDI
# controller profiles and the rekordbox reader (built by cargo).
#
# Usage:
#   bash scripts/package-native-mac.sh              # build and package
#   bash scripts/package-native-mac.sh --skip-build # package an existing build
#
# Signing: with a "Developer ID Application" identity in STEMCEL_SIGN_IDENTITY the
# bundle is signed with it; otherwise ad-hoc, which runs on the build machine and on other
# Apple Silicon Macs once Gatekeeper's quarantine flag is cleared.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
skip_build=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) skip_build=1; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This script packages the macOS app; run it on a Mac." >&2
  exit 1
fi

build_dir="${repo_root}/backend/build-native-release"
if [[ $skip_build -eq 0 ]]; then
  bash "${repo_root}/scripts/fetch-lame.sh"
  generator=()
  if command -v ninja >/dev/null 2>&1; then generator=(-G Ninja); fi
  cmake -S "${repo_root}/backend" -B "$build_dir" ${generator[@]+"${generator[@]}"} \
    -DCMAKE_BUILD_TYPE=Release -DSTEMCEL_BUILD_TESTS=OFF -DSTEMCEL_BUILD_NATIVE_APP=ON
  cmake --build "$build_dir" --config Release --parallel --target StemcelNative
fi

app="$(ls -d "${build_dir}"/StemcelNative_artefacts/Release/Stemcel.app 2>/dev/null \
  || ls -d "${build_dir}"/StemcelNative_artefacts/Stemcel.app)"
macos_dir="${app}/Contents/MacOS"

# Bundling guard: what the app loads at runtime must be inside the bundle (code in
# Contents/MacOS, data such as the MIDI controller profiles in Contents/Resources).
for required in MacOS/Stemcel MacOS/lame MacOS/libstemcel_rekordbox.dylib Resources/midi-mappings; do
  if [[ ! -e "${app}/Contents/${required}" ]]; then
    echo "Bundling guard: ${app}/Contents/${required} is missing." >&2
    exit 1
  fi
done

# The AGPL travels with the program: its text and the notice of who wrote and changed it,
# in the bundle (so the signature covers them and the .zip carries them).
for doc in LICENSE NOTICE; do
  if [[ ! -f "${repo_root}/${doc}" ]]; then
    echo "Licence guard: ${repo_root}/${doc} is missing." >&2
    exit 1
  fi
  cp "${repo_root}/${doc}" "${app}/Contents/Resources/${doc}.txt"
done

# Sign inner code first, then the bundle (hardened runtime; the entitlements come from
# juce_add_gui_app: microphone, and library validation off for third-party plugins).
identity="${STEMCEL_SIGN_IDENTITY:--}"
entitlements="$(ls "${build_dir}"/StemcelNative_artefacts/JuceLibraryCode/*.entitlements 2>/dev/null | head -1 || true)"
for code in "${macos_dir}"/*.dylib "${macos_dir}/lame"; do
  [[ -e "$code" ]] && codesign --force --options runtime --timestamp=none -s "$identity" "$code"
done
sign_args=(--force --options runtime --timestamp=none -s "$identity")
if [[ -n "$entitlements" ]]; then sign_args+=(--entitlements "$entitlements"); fi
codesign "${sign_args[@]}" "$app"
codesign --verify --strict --verbose=2 "$app"

# Prove the packaged app runs end to end from inside the bundle.
smoke_dir="$(mktemp -d)"
if ! "${macos_dir}/Stemcel" --smoke-test "$smoke_dir"; then
  echo "Packaged app failed its smoke test:" >&2
  cat "${smoke_dir}/smoke-result.txt" >&2 || true
  exit 1
fi
cat "${smoke_dir}/smoke-result.txt"

version="$(sed -n 's/^ *VERSION \([0-9.]*\).*/\1/p' "${repo_root}/backend/CMakeLists.txt" | head -1)"
version="${version:-0.0.0}"
arch="$(uname -m)"
mkdir -p "${repo_root}/dist"
base="${repo_root}/dist/Stemcel-${version}-native-mac-${arch}"

staging="$(mktemp -d)"
cp -R "$app" "$staging/"
ln -s /Applications "$staging/Applications"
# Beside the app on the disk image too, where someone opening it sees them.
cp "${repo_root}/LICENSE" "$staging/LICENSE.txt"
cp "${repo_root}/NOTICE" "$staging/NOTICE.txt"
rm -f "${base}.dmg"
hdiutil create -volname "Stemcel" -srcfolder "$staging" -ov -format UDZO "${base}.dmg"
rm -f "${base}.zip"
ditto -c -k --sequesterRsrc --keepParent "$app" "${base}.zip"
echo "Packaged:"
ls -la "${base}.dmg" "${base}.zip"
