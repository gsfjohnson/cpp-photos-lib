# libFuzzer target over every format reader and writer (Clang only):
#
#   CXX=clang++ cmake -S . -B build-fuzz -DLUMENLIB_BUILD_FUZZERS=ON
#   cmake --build build-fuzz --target fuzz_image
#   build-fuzz/fuzz/fuzz_image -max_total_time=300 tests/data
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "LUMENLIB_BUILD_FUZZERS needs Clang")
endif()
add_executable(fuzz_image fuzz_image.cpp)
target_link_libraries(fuzz_image PRIVATE lumenlib::lumenlib)
target_compile_options(fuzz_image PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(fuzz_image PRIVATE -fsanitize=fuzzer,address,undefined)
target_compile_options(lumenlib PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
