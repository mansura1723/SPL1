cmake_minimum_required(VERSION 3.16)

project(TaskFlow LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(taskflow
    src/main.cpp
    c_modules/c_test.c
    cpp_modules/cpp_test.cpp
    cpp_modules/task_management.cpp
    cpp_modules/availability_management.cpp
    tests/task_management_tests.cpp
    tests/availability_tests.cpp
)

target_include_directories(taskflow PRIVATE include)

enable_testing()
add_test(
    NAME taskflow_all_sectors
    COMMAND taskflow --test "${CMAKE_CURRENT_BINARY_DIR}/test-data"
)
