# Writes the CMake package for the ONNX Runtime slice build.sh has placed in
# PREFIX (lib/libonnxruntime.a, include/onnxruntime/). Microsoft's iOS release
# is an xcframework with no CMake package of its own; this gives a consumer the
# same onnxruntime::onnxruntime target Homebrew's and Debian's packages do.
#
#   cmake -DPREFIX=<slice prefix> -DVERSION=<x.y.z> -P install-onnxruntime.cmake
cmake_minimum_required(VERSION 3.21)

foreach(var PREFIX VERSION)
  if(NOT ${var})
    message(FATAL_ERROR "install-onnxruntime.cmake: ${var} is required")
  endif()
endforeach()

set(dir "${PREFIX}/lib/cmake/onnxruntime")
configure_file("${CMAKE_CURRENT_LIST_DIR}/onnxruntimeConfig.cmake.in"
  "${dir}/onnxruntimeConfig.cmake" @ONLY)

include(CMakePackageConfigHelpers)
write_basic_package_version_file("${dir}/onnxruntimeConfigVersion.cmake"
  VERSION "${VERSION}" COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
