# Общий вход нативных проверок. Каждый набор имеет собственную цель CTest.
cmake_minimum_required(VERSION 3.16)
project(bitvyr_native_tests LANGUAGES CXX)
enable_testing()
add_subdirectory(host)
add_subdirectory(storage)
add_subdirectory(settings)
add_executable(hid_gamepad
    ../components/board_support/test/HostHidTests.cpp
    ../components/board_support/input/HidGamepad.cpp)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/NesNativeTests.cmake")
nes_native_test_options(hid_gamepad)
add_test(NAME hid_gamepad COMMAND hid_gamepad)
