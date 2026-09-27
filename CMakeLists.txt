# photos: a permissively licensed (MIT) C++17 library for reading and writing
# photo metadata (Exif, IPTC, XMP) in JPEG, PNG, WebP, TIFF-based raw, HEIF,
# AVIF, CR3 and JPEG XL files. It has no required dependencies, so the same
# sources build for Windows, macOS, Linux, iOS and Android; zlib is optional
# (compressed PNG text chunks).
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
#
# Consumers:
#   find_package(photos CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE photos::photos)
# or add_subdirectory() this tree and link photos::photos.
cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LIBRARY_VERSION" _photos_version)
string(STRIP "${_photos_version}" _photos_version)
project(photos VERSION ${_photos_version} LANGUAGES CXX)

set(_photos_top_level OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(_photos_top_level ON)
endif()

option(PHOTOS_BUILD_TESTS "Build the unit tests" ${_photos_top_level})
option(PHOTOS_BUILD_TOOLS "Build the photos-meta command-line tool" ${_photos_top_level})
option(PHOTOS_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)
option(PHOTOS_INSTALL "Generate the install target" ${_photos_top_level})
option(PHOTOS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(PHOTOS_WITH_ZLIB "AUTO" CACHE STRING "Use zlib for compressed PNG chunks: ON, OFF or AUTO")
set_property(CACHE PHOTOS_WITH_ZLIB PROPERTY STRINGS ON OFF AUTO)

include(GNUInstallDirs)
include(GenerateExportHeader)

add_library(photos
  src/error.cpp
  src/value.cpp
  src/exif.cpp
  src/exif_tags.cpp
  src/tiff.cpp
  src/iptc.cpp
  src/photoshop.cpp
  src/xml.cpp
  src/xmp.cpp
  src/io.cpp
  src/image.cpp
  src/jpeg.cpp
  src/png.cpp
  src/webp.cpp
  src/tiff_image.cpp
  src/bmff.cpp
  src/sidecar.cpp
  src/photo_info.cpp
  src/md5.cpp
  src/zlib_support.cpp
)
add_library(photos::photos ALIAS photos)

generate_export_header(photos
  BASE_NAME PHOTOS
  EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/photos/export.hpp")
configure_file(cmake/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/include/photos/version.hpp" @ONLY)

target_compile_features(photos PUBLIC cxx_std_17)
set_target_properties(photos PROPERTIES
  CXX_EXTENSIONS OFF
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON)
if(NOT BUILD_SHARED_LIBS)
  target_compile_definitions(photos PUBLIC PHOTOS_STATIC_DEFINE)
endif()
target_include_directories(photos
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE src)

# std::filesystem needs libstdc++fs before GCC 9.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(photos PUBLIC stdc++fs)
endif()

if(MSVC)
  target_compile_options(photos PRIVATE /W4 /utf-8 /permissive-)
  if(PHOTOS_WARNINGS_AS_ERRORS)
    target_compile_options(photos PRIVATE /WX)
  endif()
else()
  target_compile_options(photos PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
  if(PHOTOS_WARNINGS_AS_ERRORS)
    target_compile_options(photos PRIVATE -Werror)
  endif()
endif()

set(PHOTOS_HAVE_ZLIB OFF)
if(NOT PHOTOS_WITH_ZLIB STREQUAL "OFF")
  if(PHOTOS_WITH_ZLIB STREQUAL "ON")
    find_package(ZLIB REQUIRED)
  else()
    find_package(ZLIB QUIET)
  endif()
  if(ZLIB_FOUND)
    set(PHOTOS_HAVE_ZLIB ON)
    target_link_libraries(photos PRIVATE ZLIB::ZLIB)
    target_compile_definitions(photos PRIVATE PHOTOS_HAVE_ZLIB=1)
  endif()
endif()
message(STATUS "photos ${PROJECT_VERSION}: zlib ${PHOTOS_HAVE_ZLIB}")

if(PHOTOS_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(PHOTOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/unit)
endif()

if(PHOTOS_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

if(PHOTOS_INSTALL)
  include(CMakePackageConfigHelpers)
  install(TARGETS photos EXPORT photosTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY include/photos DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/include/photos/export.hpp"
    "${CMAKE_CURRENT_BINARY_DIR}/include/photos/version.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/photos)
  install(EXPORT photosTargets NAMESPACE photos::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/photos)
  configure_package_config_file(cmake/photosConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/photosConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/photos)
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/photosConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/photosConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/photosConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/photos)
  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/photos)
endif()
