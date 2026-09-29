# lumenlib: a permissively licensed (MIT) C++17 library for reading and
# writing photo metadata (Exif, IPTC, XMP) in JPEG, PNG, WebP, TIFF and
# TIFF-based raw, HEIF, AVIF, CR3 and JPEG XL files. It has no required
# dependencies, so the same sources build for Windows, macOS, Linux, iOS and
# Android; zlib is optional (compressed PNG text chunks and ICC profiles).
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
#
# Consumers:
#   find_package(lumenlib CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE lumenlib::lumenlib)
# or add_subdirectory() this tree and link lumenlib::lumenlib.
cmake_minimum_required(VERSION 3.16)

# VERSION is the repository's one version: this library's, and that of the
# release packages built from it (lumenlib and lumen-ios-deps alike).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _lumenlib_version)
string(STRIP "${_lumenlib_version}" _lumenlib_version)
project(lumenlib VERSION ${_lumenlib_version} LANGUAGES CXX)

set(_lumenlib_top_level OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(_lumenlib_top_level ON)
endif()

option(LUMENLIB_BUILD_TESTS "Build the unit tests" ${_lumenlib_top_level})
option(LUMENLIB_BUILD_TOOLS "Build the lumen-meta command-line tool" ${_lumenlib_top_level})
option(LUMENLIB_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(LUMENLIB_INSTALL "Generate the install target" ${_lumenlib_top_level})
option(LUMENLIB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(LUMENLIB_WITH_ZLIB "AUTO" CACHE STRING "Use zlib for compressed PNG chunks: ON, OFF or AUTO")
set_property(CACHE LUMENLIB_WITH_ZLIB PROPERTY STRINGS ON OFF AUTO)

include(GNUInstallDirs)
include(GenerateExportHeader)

add_library(lumenlib
  src/error.cpp
  src/field_value.cpp
  src/exif.cpp
  src/exif_tags.cpp
  src/exif_describe.cpp
  src/makernote.cpp
  src/tiff.cpp
  src/iptc.cpp
  src/photoshop.cpp
  src/xml.cpp
  src/xmp.cpp
  src/io.cpp
  src/image_file.cpp
  src/jpeg.cpp
  src/png.cpp
  src/webp.cpp
  src/tiff_image.cpp
  src/raf.cpp
  src/bmff.cpp
  src/movie.cpp
  src/movie_trim.cpp
  src/sidecar.cpp
  src/photo_info.cpp
  src/md5.cpp
  src/zlib_support.cpp
)
add_library(lumenlib::lumenlib ALIAS lumenlib)

generate_export_header(lumenlib
  BASE_NAME LUMENLIB
  EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/lumenlib/export.hpp")
configure_file(cmake/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/include/lumenlib/version.hpp" @ONLY)

target_compile_features(lumenlib PUBLIC cxx_std_17)
set_target_properties(lumenlib PROPERTIES
  CXX_EXTENSIONS OFF
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON)
if(NOT BUILD_SHARED_LIBS)
  target_compile_definitions(lumenlib PUBLIC LUMENLIB_STATIC_DEFINE)
endif()
target_include_directories(lumenlib
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE src)

# std::filesystem needs libstdc++fs before GCC 9.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(lumenlib PUBLIC stdc++fs)
endif()

if(MSVC)
  target_compile_options(lumenlib PRIVATE /W4 /utf-8 /permissive-)
  if(LUMENLIB_WARNINGS_AS_ERRORS)
    target_compile_options(lumenlib PRIVATE /WX)
  endif()
else()
  target_compile_options(lumenlib PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  if(LUMENLIB_WARNINGS_AS_ERRORS)
    target_compile_options(lumenlib PRIVATE -Werror)
  endif()
endif()

set(LUMENLIB_HAVE_ZLIB OFF)
if(NOT LUMENLIB_WITH_ZLIB STREQUAL "OFF")
  if(LUMENLIB_WITH_ZLIB STREQUAL "ON")
    find_package(ZLIB REQUIRED)
  else()
    find_package(ZLIB QUIET)
  endif()
  if(ZLIB_FOUND)
    set(LUMENLIB_HAVE_ZLIB ON)
    target_link_libraries(lumenlib PRIVATE ZLIB::ZLIB)
    target_compile_definitions(lumenlib PRIVATE LUMENLIB_HAVE_ZLIB=1)
  endif()
endif()
message(STATUS "lumenlib ${PROJECT_VERSION}: zlib ${LUMENLIB_HAVE_ZLIB}")

if(LUMENLIB_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(LUMENLIB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/unit)
endif()

if(LUMENLIB_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

if(LUMENLIB_INSTALL)
  include(CMakePackageConfigHelpers)
  install(TARGETS lumenlib EXPORT lumenlibTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY include/lumenlib DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/include/lumenlib/export.hpp"
    "${CMAKE_CURRENT_BINARY_DIR}/include/lumenlib/version.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lumenlib)
  install(EXPORT lumenlibTargets NAMESPACE lumenlib::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lumenlib)
  configure_package_config_file(cmake/lumenlibConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/lumenlibConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lumenlib)
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/lumenlibConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/lumenlibConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/lumenlibConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lumenlib)
  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/lumenlib)
endif()
