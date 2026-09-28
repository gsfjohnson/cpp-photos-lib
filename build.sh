#!/usr/bin/env bash
# Builds lumen-ios-deps: the iOS prefixes Lumen (../cpp-photos) links for what
# redwain's iOS packages do not carry. Each slice is one install tree and one
# tarball, lumen-ios-deps-<version>-<slice>.tar.gz, holding
#
#   lumenlib   static, this repository's library (VERSION is its version and
#              the package's), with the SDK's zlib
#   exiv2      static, with expat for XMP and the SDK's zlib for PNG
#   libwebp    static: libwebp, libsharpyuv, libwebpdecoder, libwebpdemux,
#              libwebpmux
#   ONNX Runtime  Microsoft's prebuilt (the onnxruntime-c pod), this slice of
#              its xcframework as a static library, with a CMake package
#              (onnxruntime::onnxruntime) written here
#
#   ./build.sh fetch              download and verify the pinned sources
#   ./build.sh <slice>...         build, check and package slices
#   ./build.sh all                every slice
#
# Slices: arm64 (devices), sim-arm64 (Simulator, Apple silicon Macs),
# sim-x86_64 (Simulator, Intel Macs). Building needs a Mac with Xcode and
# cmake; fetch needs only curl and the internet, so an offline Mac is given a
# sources/ directory fetched elsewhere. The generator is CMake's default unless
# CMAKE_GENERATOR says otherwise; JOBS caps the parallel build.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
VERSION=$(tr -d '[:space:]' < "$ROOT/VERSION")
DEPLOYMENT_TARGET=16.0 # Lumen's iOS deployment target (pm/ADD_IOS.md §7)
SLICES="arm64 sim-arm64 sim-x86_64"

# ---- Pinned sources -----------------------------------------------------------
# A bump changes the version, URL, file name and hash together, and VERSION.
# exiv2 publishes no source tarball, so its pin is GitHub's tag archive: should
# GitHub ever regenerate it differently, fetch fails on the hash rather than
# building something else.
EXPAT_VERSION=2.8.5
EXPAT_URL=https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-2.8.5.tar.gz
EXPAT_FILE=expat-2.8.5.tar.gz
EXPAT_SHA256=920dde485e15eda0cce8d2310b41d492c534e5e3d89ad407a0b4176dd2ff88fe

EXIV2_VERSION=0.28.9
EXIV2_URL=https://github.com/Exiv2/exiv2/archive/refs/tags/v0.28.9.tar.gz
EXIV2_FILE=exiv2-0.28.9.tar.gz
EXIV2_SHA256=700b76b97695b2fab4ef8c79619c68ae57d09e0c130724791cafbd39e0eb4aef

WEBP_VERSION=1.6.0
WEBP_URL=https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz
WEBP_FILE=libwebp-1.6.0.tar.gz
WEBP_SHA256=e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564

ORT_VERSION=1.30.0
ORT_URL=https://download.onnxruntime.ai/pod-archive-onnxruntime-c-1.30.0.zip
ORT_FILE=onnxruntime-c-1.30.0.zip
ORT_SHA256=e6f1670c14406fd9f082bb400ab197a9b0a9646058ca6366e440642e2b54a2ea

SOURCES="EXPAT EXIV2 WEBP ORT"

SRC="$ROOT/sources"
BUILD="$ROOT/build"
STAGE="$ROOT/stage"
DIST="$ROOT/dist"

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'build.sh: %s\n' "$*" >&2; exit 1; }

sha256() {
  if command -v shasum > /dev/null; then
    shasum -a 256 "$1" | cut -d' ' -f1
  else
    sha256sum "$1" | cut -d' ' -f1
  fi
}

archive_of() { local file_var="$1_FILE"; echo "${!file_var}"; }

# ---- fetch --------------------------------------------------------------------

fetch() {
  mkdir -p "$SRC"
  local name url sum file
  for name in $SOURCES; do
    url="${name}_URL"; url="${!url}"
    sum="${name}_SHA256"; sum="${!sum}"
    file="$SRC/$(archive_of "$name")"
    if [ -f "$file" ] && [ "$(sha256 "$file")" = "$sum" ]; then
      echo "ok       $(basename "$file")"
      continue
    fi
    echo "fetching $url"
    curl -fsSL --retry 3 -o "$file.part" "$url"
    [ "$(sha256 "$file.part")" = "$sum" ] ||
      die "$(basename "$file"): SHA-256 mismatch (expected $sum)"
    mv "$file.part" "$file"
  done
}

verify_sources() {
  local name sum file
  for name in $SOURCES; do
    sum="${name}_SHA256"; sum="${!sum}"
    file="$SRC/$(archive_of "$name")"
    [ -f "$file" ] || die "missing $file: run ./build.sh fetch (or copy sources/ from a machine that can)"
    [ "$(sha256 "$file")" = "$sum" ] || die "$(basename "$file"): SHA-256 mismatch (expected $sum)"
  done
}

# Unpacked once per run into build/src; the builds are out of source.
unpack() {
  log "Unpacking sources"
  rm -rf "$BUILD/src"
  mkdir -p "$BUILD/src"
  tar -xzf "$SRC/$(archive_of EXPAT)" -C "$BUILD/src"
  tar -xzf "$SRC/$(archive_of EXIV2)" -C "$BUILD/src"
  tar -xzf "$SRC/$(archive_of WEBP)" -C "$BUILD/src"
  mkdir "$BUILD/src/onnxruntime-c-$ORT_VERSION"
  unzip -q "$SRC/$(archive_of ORT)" -d "$BUILD/src/onnxruntime-c-$ORT_VERSION"
}

# ---- One slice ----------------------------------------------------------------

build_slice() {
  local slice=$1 sysroot arch ort_slice platform
  case "$slice" in
    arm64)      sysroot=iphoneos;        arch=arm64;  ort_slice=ios-arm64;                  platform=IOS ;;
    sim-arm64)  sysroot=iphonesimulator; arch=arm64;  ort_slice=ios-arm64_x86_64-simulator; platform=IOSSIMULATOR ;;
    sim-x86_64) sysroot=iphonesimulator; arch=x86_64; ort_slice=ios-arm64_x86_64-simulator; platform=IOSSIMULATOR ;;
    *) die "unknown slice '$slice' (one of: $SLICES)" ;;
  esac

  local pkg="lumen-ios-deps-$VERSION-$slice"
  local prefix="$STAGE/$pkg"
  local work="$BUILD/$slice"
  local jobs=${JOBS:-$(sysctl -n hw.ncpu)}
  rm -rf "$prefix" "$work"
  mkdir -p "$prefix" "$work" "$DIST"

  # CMAKE_FIND_ROOT_PATH is how an iOS configure finds a prefix: once CMake
  # re-roots its searches to the SDK it skips plain CMAKE_PREFIX_PATH entries.
  local ios=(
    -DCMAKE_SYSTEM_NAME=iOS
    "-DCMAKE_OSX_SYSROOT=$sysroot"
    "-DCMAKE_OSX_ARCHITECTURES=$arch"
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=OFF
    "-DCMAKE_INSTALL_PREFIX=$prefix"
    -DCMAKE_INSTALL_LIBDIR=lib
    "-DCMAKE_FIND_ROOT_PATH=$prefix"
  )

  log "[$slice] expat $EXPAT_VERSION"
  cmake -S "$BUILD/src/expat-$EXPAT_VERSION" -B "$work/expat" "${ios[@]}" \
    -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF \
    -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_BUILD_FUZZERS=OFF
  cmake --build "$work/expat" --parallel "$jobs"
  cmake --install "$work/expat"

  # Brotli (JPEG XL's compressed boxes) and inih (the ~/.exiv2 lens-name file)
  # are left out: neither exists for iOS, and a static exiv2 package refuses to
  # load without them when they are in.
  log "[$slice] exiv2 $EXIV2_VERSION"
  cmake -S "$BUILD/src/exiv2-$EXIV2_VERSION" -B "$work/exiv2" "${ios[@]}" \
    -DEXIV2_ENABLE_XMP=ON -DEXIV2_ENABLE_PNG=ON -DEXIV2_ENABLE_BMFF=ON \
    -DEXIV2_ENABLE_VIDEO=ON -DEXIV2_ENABLE_BROTLI=OFF -DEXIV2_ENABLE_INIH=OFF \
    -DEXIV2_ENABLE_NLS=OFF -DEXIV2_ENABLE_WEBREADY=OFF -DEXIV2_ENABLE_CURL=OFF \
    -DEXIV2_BUILD_EXIV2_COMMAND=OFF -DEXIV2_BUILD_SAMPLES=OFF \
    -DEXIV2_BUILD_UNIT_TESTS=OFF -DEXIV2_BUILD_DOC=OFF
  cmake --build "$work/exiv2" --parallel "$jobs"
  cmake --install "$work/exiv2"

  log "[$slice] libwebp $WEBP_VERSION"
  cmake -S "$BUILD/src/libwebp-$WEBP_VERSION" -B "$work/libwebp" "${ios[@]}" \
    -DWEBP_BUILD_LIBWEBPMUX=ON -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF \
    -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
    -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF \
    -DWEBP_BUILD_EXTRAS=OFF
  cmake --build "$work/libwebp" --parallel "$jobs"
  cmake --install "$work/libwebp"

  log "[$slice] lumenlib $VERSION"
  cmake -S "$ROOT" -B "$work/lumenlib" "${ios[@]}" \
    -DLUMENLIB_WITH_ZLIB=ON -DLUMENLIB_BUILD_TESTS=OFF -DLUMENLIB_BUILD_TOOLS=OFF \
    -DLUMENLIB_WARNINGS_AS_ERRORS=ON
  cmake --build "$work/lumenlib" --parallel "$jobs"
  cmake --install "$work/lumenlib"

  log "[$slice] ONNX Runtime $ORT_VERSION ($ort_slice, $arch)"
  local ort="$BUILD/src/onnxruntime-c-$ORT_VERSION"
  local ort_bin="$ort/onnxruntime.xcframework/$ort_slice/onnxruntime.framework/onnxruntime"
  lipo -thin "$arch" "$ort_bin" -output "$prefix/lib/libonnxruntime.a"
  mkdir -p "$prefix/include/onnxruntime"
  cp "$ort"/Headers/*.h "$prefix/include/onnxruntime/"
  cmake "-DPREFIX=$prefix" "-DVERSION=$ORT_VERSION" -P "$ROOT/cmake/install-onnxruntime.cmake"

  log "[$slice] Licences and notes"
  local lic="$prefix/share/licenses"
  mkdir -p "$lic/expat" "$lic/exiv2" "$lic/libwebp" "$lic/onnxruntime"
  cp "$BUILD/src/expat-$EXPAT_VERSION/COPYING" "$lic/expat/"
  cp "$BUILD/src/exiv2-$EXIV2_VERSION/COPYING" "$lic/exiv2/"
  cp "$BUILD/src/libwebp-$WEBP_VERSION/COPYING" "$BUILD/src/libwebp-$WEBP_VERSION/PATENTS" "$lic/libwebp/"
  cp "$ort/LICENSE" "$lic/onnxruntime/"
  rm -rf "$prefix/share/doc" "$prefix/share/man"
  cp "$ROOT/docs/ios-deps.md" "$prefix/README.md"
  {
    echo "lumen-ios-deps $VERSION ($slice: $sysroot $arch, iOS $DEPLOYMENT_TARGET)"
    echo "built $(date -u +%Y-%m-%dT%H:%M:%SZ) with $(xcodebuild -version | tr '\n' ' ')"
    echo "commit $(git -C "$ROOT" rev-parse --short HEAD 2> /dev/null || echo unknown)"
    echo
    echo "lumenlib  $VERSION  this repository, at the commit above"
    local name version url sum
    for name in $SOURCES; do
      version="${name}_VERSION" url="${name}_URL" sum="${name}_SHA256"
      echo "$(archive_of "$name")  ${!version}  ${!url}  sha256 ${!sum}"
    done
  } > "$prefix/VERSIONS"

  # pkg-config files name the staging prefix; make them relative to where they
  # are unpacked.
  local pc
  for pc in "$prefix"/lib/pkgconfig/*.pc; do
    sed -i '' "s|$prefix|\${pcfiledir}/../..|g" "$pc"
  done

  check_slice "$slice" "$arch" "$platform" "$prefix"

  log "[$slice] Package"
  tar -czf "$DIST/$pkg.tar.gz" -C "$STAGE" "$pkg"
  echo "$DIST/$pkg.tar.gz"
}

# ---- Checks -------------------------------------------------------------------

check_slice() {
  local slice=$1 arch=$2 platform=$3 prefix=$4

  log "[$slice] Check archives"
  local lib archs
  for lib in "$prefix"/lib/*.a; do
    archs=$(lipo -archs "$lib")
    echo "$(basename "$lib"): $archs"
    [ "$archs" = "$arch" ] || die "$lib is '$archs', not $arch"
    ar -t "$lib" > /dev/null || die "$lib is not a static archive"
  done

  # Nothing may name the build machine: every package file is relative.
  if grep -rlI -e "$prefix" -e "$BUILD" "$prefix/lib/cmake" "$prefix/lib/pkgconfig" "$prefix/include"; then
    die "the files above hold absolute build paths"
  fi

  # The consumer finds every package the way Lumen's iOS configure does and
  # links each library for real. Nothing iOS runs here: that it links is the
  # check, and vtool shows the platform and minimum OS it was linked for.
  log "[$slice] Smoke test (link only)"
  local sysroot=iphoneos
  [ "$platform" = IOSSIMULATOR ] && sysroot=iphonesimulator
  local consumer="$BUILD/$slice/consumer"
  cmake -S "$ROOT/tests/consumer" -B "$consumer" \
    -DCMAKE_SYSTEM_NAME=iOS "-DCMAKE_OSX_SYSROOT=$sysroot" "-DCMAKE_OSX_ARCHITECTURES=$arch" \
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=$DEPLOYMENT_TARGET" -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_FIND_ROOT_PATH=$prefix" \
    "-Dexiv2_DIR=$prefix/lib/cmake/exiv2" \
    "-DWebP_DIR=$prefix/share/WebP/cmake" \
    "-Donnxruntime_DIR=$prefix/lib/cmake/onnxruntime" \
    "-Dlumenlib_DIR=$prefix/lib/cmake/lumenlib" \
    "-DEXPECT_EXIV2_VERSION=$EXIV2_VERSION" "-DEXPECT_WEBP_VERSION=$WEBP_VERSION" \
    "-DEXPECT_ORT_VERSION=$ORT_VERSION" "-DEXPECT_LUMENLIB_VERSION=$VERSION"
  # A library built for a newer iOS than the deployment target only warns at
  # link time; here it fails.
  local out="$consumer/link.log"
  cmake --build "$consumer" --parallel "${JOBS:-$(sysctl -n hw.ncpu)}" 2>&1 | tee "$out"
  if grep -i "built for newer" "$out"; then
    die "a library above needs a newer iOS than $DEPLOYMENT_TARGET"
  fi
  local bin="$consumer/lumen_ios_deps_consumer.app/lumen_ios_deps_consumer"
  vtool -show-build "$bin"
  vtool -show-build "$bin" | grep -q "platform $platform\$" || die "$bin is not linked for $platform"
  vtool -show-build "$bin" | grep -q "minos $DEPLOYMENT_TARGET\$" || die "$bin is not linked for iOS $DEPLOYMENT_TARGET"
}

# ---- Main ---------------------------------------------------------------------

[ $# -gt 0 ] || { sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

if [ "$1" = fetch ]; then
  fetch
  exit 0
fi

[ "$(uname -s)" = Darwin ] || die "building needs macOS with Xcode (fetch works anywhere)"
command -v cmake > /dev/null || die "cmake is not on PATH"
xcrun --sdk iphoneos --show-sdk-path > /dev/null || die "no iOS SDK: install Xcode"

slices=("$@")
[ "$1" = all ] && read -r -a slices <<< "$SLICES"
for slice in "${slices[@]}"; do
  case " $SLICES " in *" $slice "*) ;; *) die "unknown slice '$slice' (one of: $SLICES)" ;; esac
done

verify_sources
unpack
for slice in "${slices[@]}"; do
  build_slice "$slice"
done
log "Done: $(cd "$DIST" && ls lumen-ios-deps-"$VERSION"-*.tar.gz | tr '\n' ' ')"
