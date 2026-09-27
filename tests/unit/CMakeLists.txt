add_executable(photos_tests
  main.cpp
  test_value.cpp
  test_exif.cpp
  test_iptc.cpp
  test_xmp.cpp
  test_formats.cpp
  test_photo_info.cpp
  test_robustness.cpp)
target_link_libraries(photos_tests PRIVATE photos::photos)
target_compile_definitions(photos_tests PRIVATE PHOTOS_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
if(MSVC)
  target_compile_options(photos_tests PRIVATE /utf-8)
endif()
add_test(NAME photos_tests COMMAND photos_tests)
