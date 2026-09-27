# Builds against an installed photos package, as an application would:
#
#   cmake --install <build> --prefix <prefix>
#   cmake -S tests/find_package -B <dir> -DCMAKE_PREFIX_PATH=<prefix>
#   cmake --build <dir> && <dir>/find_package_consumer <image>
cmake_minimum_required(VERSION 3.16)
project(photos_find_package_consumer LANGUAGES CXX)

find_package(photos CONFIG REQUIRED)
message(STATUS "photos ${photos_VERSION} (${photos_DIR})")

add_executable(find_package_consumer consumer.cpp)
target_link_libraries(find_package_consumer PRIVATE photos::photos)
