cmake_minimum_required(VERSION 3.16)
project(nes_storage_tests LANGUAGES CXX)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/NesNativeTests.cmake")

set(STORAGE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../app/nes/storage")
set(FIXTURE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
add_custom_command(
    OUTPUT "${FIXTURE_DIRECTORY}/fixture-summary.json" "${FIXTURE_DIRECTORY}/FixtureValues.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/fixtures.py" --output "${FIXTURE_DIRECTORY}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/fixtures.py"
    VERBATIM)
add_custom_target(storage_fixtures ALL DEPENDS "${FIXTURE_DIRECTORY}/fixture-summary.json" "${FIXTURE_DIRECTORY}/FixtureValues.h")

function(storage_target target)
    nes_native_test_options(${target})
    target_include_directories(${target} PRIVATE "${STORAGE_SOURCE}" "${FIXTURE_DIRECTORY}")
endfunction()

add_executable(storage_library_tests LibraryTests.cpp "${STORAGE_SOURCE}/GameLibrary.cpp" "${STORAGE_SOURCE}/ZipArchive.cpp")
target_compile_definitions(storage_library_tests PRIVATE NES_LIBRARY_TEST_ALLOCATOR=1)
storage_target(storage_library_tests)
add_dependencies(storage_library_tests storage_fixtures)
add_test(NAME storage_library COMMAND storage_library_tests "${FIXTURE_DIRECTORY}")

# Регрессия подменяет только ZIP I/O; настоящая production library остаётся прежней.
add_executable(storage_prefix_io_tests PrefixIoTests.cpp "${STORAGE_SOURCE}/GameLibrary.cpp")
storage_target(storage_prefix_io_tests)
add_dependencies(storage_prefix_io_tests storage_fixtures)
add_test(NAME storage_prefix_io COMMAND storage_prefix_io_tests "${FIXTURE_DIRECTORY}/prefix-io")

add_executable(storage_zip_tests ZipTests.cpp "${STORAGE_SOURCE}/ZipArchive.cpp")
storage_target(storage_zip_tests)
add_test(NAME storage_zip_reference COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/zip_reference.py"
    --runner "$<TARGET_FILE:storage_zip_tests>" --output "${CMAKE_CURRENT_BINARY_DIR}/zip-reference")

set_tests_properties(storage_library storage_prefix_io PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
set_tests_properties(storage_zip_reference PROPERTIES TIMEOUT 180)
