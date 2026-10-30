#!/usr/bin/env bash
# Build the bundled LAME MP3 encoder/decoder for macOS from source.
#
# Stemcel spawns `lame` as a child process for MP3 export and import (see
# backend/third_party/lame/README.md). On Windows, scripts/Fetch-Lame.ps1
# downloads a prebuilt lame.exe; there is no equivalent standard binary
# distribution for macOS, so this script builds the pinned upstream release
# instead — verifying the tarball's SHA-256 first — and installs the result at
# backend/third_party/lame/lame, where CMake expects it.
#
# The binary is linked statically against libmp3lame, so it is one
# self-contained file that ships next to the backend.
#
# Usage:
#   scripts/fetch-lame.sh                         # build for this machine
#   scripts/fetch-lame.sh --arch "arm64 x86_64"   # universal binary
#   scripts/fetch-lame.sh --force                 # rebuild an existing binary
#
# Requires the Xcode command-line tools (clang, make). LAME is
# LGPL-2.1-or-later and runs as a separate process; see THIRD_PARTY_LICENSES.md.
set -euo pipefail

LAME_VERSION="3.100"
LAME_SHA256="ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e"
LAME_URL="https://downloads.sourceforge.net/project/lame/lame/${LAME_VERSION}/lame-${LAME_VERSION}.tar.gz"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="${repo_root}/backend/third_party/lame/lame"

force=0
archs=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --force) force=1; shift ;;
    --arch) archs="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -x "$dest" && $force -eq 0 ]]; then
  echo "lame already present at $dest (use --force to rebuild)"
  "$dest" --version | head -n 1
  exit 0
fi

# Stage everything in a temp dir so a failed build never leaves a half-written binary.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Downloading $LAME_URL"
curl -fsSL --retry 3 -o "$work/lame.tar.gz" "$LAME_URL"
actual="$(shasum -a 256 "$work/lame.tar.gz" | awk '{print $1}')"
if [[ "$actual" != "$LAME_SHA256" ]]; then
  echo "SHA-256 mismatch for lame-${LAME_VERSION}.tar.gz" >&2
  echo "  expected: $LAME_SHA256" >&2
  echo "  actual:   $actual" >&2
  exit 1
fi
echo "SHA-256 verified."

tar -xzf "$work/lame.tar.gz" -C "$work"
src="$work/lame-${LAME_VERSION}"

# 3.100's export list names `lame_init_old`, which the library no longer defines; the
# macOS linker rejects that when building the shared library. Only the static library
# is built here, but drop the stale entry so the build never depends on that detail.
sed -i.bak '/^lame_init_old$/d' "$src/include/libmp3lame.sym"

# Match the backend's deployment target (backend/CMakeLists.txt) so the binary runs
# everywhere the app does.
cflags="-O2 -mmacosx-version-min=${MACOSX_DEPLOYMENT_TARGET:-14.0}"
for arch in $archs; do
  cflags="$cflags -arch $arch"
done

jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
# LAME 3.100's config.guess/config.sub predate Apple Silicon (and don't know the `arm64`
# spelling), so name the build machine explicitly rather than let configure guess.
case "$(uname -m)" in
  arm64) build_triplet="aarch64-apple-darwin" ;;
  *) build_triplet="$(uname -m)-apple-darwin" ;;
esac
(
  cd "$src"
  ./configure \
    --build="$build_triplet" \
    --disable-shared \
    --enable-static \
    --disable-dependency-tracking \
    --disable-debug \
    CFLAGS="$cflags" \
    LDFLAGS="$cflags" >"$work/configure.log" 2>&1 || {
      cat "$work/configure.log" >&2
      exit 1
    }
  make -j"$jobs" >"$work/make.log" 2>&1 || {
    tail -n 60 "$work/make.log" >&2
    exit 1
  }
)

install -m 0755 "$src/frontend/lame" "$dest"
echo "Installed $dest"
file "$dest" || true
"$dest" --version | head -n 1
