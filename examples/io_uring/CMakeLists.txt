cmake_minimum_required(VERSION 3.15)
project(capio_io_uring_example CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(liburing REQUIRED IMPORTED_TARGET liburing)

add_executable(one_to_one one_to_one.cpp)
target_link_libraries(one_to_one PRIVATE PkgConfig::liburing)
