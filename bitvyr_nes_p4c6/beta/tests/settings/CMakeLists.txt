cmake_minimum_required(VERSION 3.16)
project(nes_settings_tests LANGUAGES CXX)
enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/NesNativeTests.cmake")

set(SETTINGS_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../app/nes/settings")
add_executable(settings_value_tests SettingsTests.cpp)
target_include_directories(settings_value_tests PRIVATE "${SETTINGS_SOURCE}")
nes_native_test_options(settings_value_tests)
add_test(NAME settings_value COMMAND settings_value_tests)

# Компилируется настоящий адаптер NVS с узким fake API; платы и SDK здесь нет.
add_executable(settings_store_tests SettingsStoreTests.cpp "${SETTINGS_SOURCE}/SettingsStore.cpp")
target_include_directories(settings_store_tests PRIVATE "${SETTINGS_SOURCE}" "${CMAKE_CURRENT_LIST_DIR}/fake")
nes_native_test_options(settings_store_tests)
add_test(NAME settings_store COMMAND settings_store_tests)
