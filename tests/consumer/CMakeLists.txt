# Builds against one installed slice exactly as Lumen's iOS configure does:
#
#   cmake -S tests/consumer -B <dir> -DCMAKE_SYSTEM_NAME=iOS ... \
#     -DCMAKE_FIND_ROOT_PATH=<prefix> -Dexiv2_DIR=<prefix>/lib/cmake/exiv2 \
#     -DWebP_DIR=<prefix>/share/WebP/cmake -Donnxruntime_DIR=<prefix>/lib/cmake/onnxruntime
#
# build.sh runs it for every slice. It is linked, not run: the program touches
# enough of each library (XMP through expat, PNG through zlib, WebP encoding
# through sharpyuv, an ONNX Runtime session with Core ML) that a missing
# dependency or framework fails the link.
cmake_minimum_required(VERSION 3.21)
project(lumen_ios_deps_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(exiv2 CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(onnxruntime CONFIG REQUIRED)

# The versions build.sh pinned are the versions found.
foreach(pkg IN ITEMS exiv2:EXIV2 WebP:WEBP onnxruntime:ORT)
  string(REPLACE ":" ";" pkg "${pkg}")
  list(GET pkg 0 name)
  list(GET pkg 1 key)
  message(STATUS "${name} ${${name}_VERSION} (${${name}_DIR})")
  if(DEFINED EXPECT_${key}_VERSION AND NOT "${${name}_VERSION}" VERSION_EQUAL "${EXPECT_${key}_VERSION}")
    message(FATAL_ERROR "${name}: found ${${name}_VERSION}, expected ${EXPECT_${key}_VERSION}")
  endif()
endforeach()

add_executable(lumen_ios_deps_consumer MACOSX_BUNDLE consumer.cpp)
set_target_properties(lumen_ios_deps_consumer PROPERTIES
  MACOSX_BUNDLE_GUI_IDENTIFIER com.redwain.lumen-ios-deps-consumer)
target_link_libraries(lumen_ios_deps_consumer PRIVATE
  Exiv2::exiv2lib WebP::webp WebP::webpdemux WebP::libwebpmux onnxruntime::onnxruntime)
