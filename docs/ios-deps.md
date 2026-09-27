# lumen-ios-deps

> exiv2 is here because Lumen uses it today. The `lumenlib` library in this
> repository ([README](../README.md)) replaces it with MIT-licensed code
> ([migrating.md](migrating.md)); once Lumen has moved over, exiv2 and expat
> can leave these packages.

The iOS libraries Lumen ([`../cpp-photos`](../../cpp-photos)) links that
redwain's iOS packages do not carry, built once per slice into a prefix an iOS
configure is pointed at (`pm/ADD_IOS.md` §3 in Lumen).

| Library | Version | Built as | Licence |
| --- | --- | --- | --- |
| exiv2 | 0.28.9 | static, from source | GPL-2.0-or-later |
| expat (exiv2's XMP parser) | 2.8.5 | static, from source | MIT |
| libwebp (with sharpyuv, demux, mux, decoder) | 1.6.0 | static, from source | BSD-3-Clause (+ PATENTS) |
| ONNX Runtime | 1.30.0 | Microsoft's prebuilt, the `onnxruntime-c` pod's xcframework | MIT |

zlib (exiv2's PNG support) and iconv come from the iOS SDK.

## Packages

| Package | Slice | Runs on |
| --- | --- | --- |
| `lumen-ios-deps-X.Y.Z-arm64.tar.gz` | iphoneos arm64 | iOS 16 and later (devices) |
| `lumen-ios-deps-X.Y.Z-sim-arm64.tar.gz` | iphonesimulator arm64 | Simulator on Apple silicon Macs |
| `lumen-ios-deps-X.Y.Z-sim-x86_64.tar.gz` | iphonesimulator x86_64 | Simulator on Intel Macs |

Each is one install tree:

```
lib/                  libexiv2.a libexpat.a libwebp.a libsharpyuv.a libwebpdecoder.a
                      libwebpdemux.a libwebpmux.a libonnxruntime.a
lib/cmake/            exiv2/ expat-X.Y.Z/ onnxruntime/
lib/pkgconfig/        exiv2.pc expat.pc libwebp.pc libsharpyuv.pc ... (relocatable)
share/WebP/cmake/     WebPConfig.cmake
include/              exiv2/ webp/ expat.h expat_config.h expat_external.h
include/onnxruntime/  onnxruntime_c_api.h onnxruntime_cxx_api.h coreml_provider_factory.h ...
share/licenses/       each library's licence
VERSIONS              the pinned sources, their SHA-256 and the Xcode that built them
```

The deployment target is iOS 16.0, Lumen's. Device and simulator arm64 are one
architecture for two platforms, so they cannot share an archive; that is why
the packages are per slice and not an xcframework.

## Using it

Unpack each slice under `~/opt/lumen-ios-deps-<slice>`. When CMake
cross-compiles for iOS it re-roots its searches to the SDK and skips plain
`CMAKE_PREFIX_PATH` entries, so an iOS configure names the prefix with
`CMAKE_FIND_ROOT_PATH` and each package's directory explicitly. With redwain's
slice beside it:

```bash
deps=$HOME/opt/lumen-ios-deps-arm64
redwain=$HOME/opt/redwain-ios-arm64
cmake -B build-ios-arm64 -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  "-DCMAKE_FIND_ROOT_PATH=$deps;$redwain" \
  -Dredwain_DIR=$redwain/lib/cmake/redwain \
  -Dexiv2_DIR=$deps/lib/cmake/exiv2 \
  -DWebP_DIR=$deps/share/WebP/cmake \
  -Donnxruntime_DIR=$deps/lib/cmake/onnxruntime
```

```cmake
find_package(exiv2 CONFIG REQUIRED)        # Exiv2::exiv2lib (finds EXPAT, ZLIB, Iconv itself)
find_package(WebP CONFIG REQUIRED)         # WebP::webp, WebP::sharpyuv, WebP::webpdemux, ...
find_package(onnxruntime CONFIG REQUIRED)  # onnxruntime::onnxruntime
```

`onnxruntime::onnxruntime` is a package written here (Microsoft ships none for
iOS). It is the static library from the xcframework's slice, thinned to one
architecture. It brings the frameworks Microsoft's build calls into:
Foundation, Core ML, UIKit (`UIDevice`) and Network (`nw_path_monitor`).
Its headers are included by bare name (`<onnxruntime_cxx_api.h>`), as with
Homebrew's package.

### How exiv2 differs from the desktop's

Brotli and inih are left out: neither has an iOS build, and a static exiv2
package refuses to load without them when they are in. Without Brotli, exiv2
cannot read metadata from JPEG XL files whose boxes are Brotli-compressed
(`brob`). Without inih, exiv2 does not read the `~/.exiv2` file of
user-defined lens names. Everything else is exiv2's default: XMP, PNG, BMFF
(HEIC, AVIF, CR3), video, lens data and filesystem access.

## Building

`build.sh` holds every pin (version, URL, file name, SHA-256). It needs a Mac
with Xcode and cmake to build; fetching needs only curl and the internet.

```bash
./build.sh fetch                 # download and verify into sources/
./build.sh all                   # or: ./build.sh arm64 sim-arm64 sim-x86_64
```

For each slice it builds expat, exiv2 and libwebp, takes ONNX Runtime's slice
from the xcframework, writes the licences and `VERSIONS`, and then checks the
result:

- every archive is a static library of this slice's one architecture;
- no package file (CMake, pkg-config, headers) names the build machine;
- `tests/consumer` configures against the prefix the way Lumen does, finds the
  versions pinned, and links a program that uses each library (XMP through
  expat, a PNG through zlib, a WebP encode with sharp YUV, an ONNX Runtime
  session with Core ML);
- no library needs a newer iOS than 16.0, and the program is linked for the
  slice's platform (`vtool`).

Nothing iOS runs on the build machine, so the consumer is linked, not run.
The packages land in `dist/`, the prefixes in `stage/`.

`JOBS` caps the parallel build (default: every core). `CMAKE_GENERATOR` picks
the generator (default: CMake's; the release workflow uses Ninja).

On a Mac without the internet, run `./build.sh fetch` elsewhere and copy
`sources/` over with the tree: the build only verifies what is there.

## Updating a library

Change its version, URL, file name and hash in `build.sh`, the table above,
and `VERSION` (a new minor for a library upgrade, a new patch for a rebuild),
then build every slice. Lumen pins this package's version in two places
(`release.yml` and `cmake/Dependencies.cmake`'s iOS branch), so a bump there is
a one-line change once the release is out.

## Releasing

Publishing a GitHub release tagged `vX.Y.Z` (matching `VERSION`) runs
`.github/workflows/release.yml`: the three slices build on `macos-latest` and
their tarballs are attached to the release with a `SHA256SUMS` file. A manual
dispatch runs the same builds and keeps the packages as workflow artifacts
only.
