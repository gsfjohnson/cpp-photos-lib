add_executable(lumenlib_tests
  main.cpp
  test_value.cpp
  test_exif.cpp
  test_iptc.cpp
  test_xmp.cpp
  test_formats.cpp
  test_photo_info.cpp
  test_makernote.cpp
  test_samples.cpp
  test_robustness.cpp)
target_link_libraries(lumenlib_tests PRIVATE lumenlib::lumenlib)
target_compile_definitions(lumenlib_tests PRIVATE LUMENLIB_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
if(MSVC)
  target_compile_options(lumenlib_tests PRIVATE /utf-8)
endif()
add_test(NAME lumenlib_tests COMMAND lumenlib_tests)
