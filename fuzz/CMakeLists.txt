# libFuzzer target over every format reader and writer (Clang only):
#
#   CXX=clang++ cmake -S . -B build-fuzz -DPHOTOS_BUILD_FUZZERS=ON
#   cmake --build build-fuzz --target fuzz_image
#   build-fuzz/fuzz/fuzz_image -max_total_time=300 tests/data
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "PHOTOS_BUILD_FUZZERS needs Clang")
endif()
add_executable(fuzz_image fuzz_image.cpp)
target_link_libraries(fuzz_image PRIVATE photos::photos)
target_compile_options(fuzz_image PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(fuzz_image PRIVATE -fsanitize=fuzzer,address,undefined)
target_compile_options(photos PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
