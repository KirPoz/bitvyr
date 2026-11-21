cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "Open the pinned ESP-IDF 6.1 environment first. Native: cmake -S tests -B build/native")
endif()

include("$ENV{IDF_PATH}/tools/cmake/version.cmake")
set(NES_IDF_VERSION "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 1)
    message(FATAL_ERROR "Bitvyr P4+C6 beta requires the pinned ESP-IDF 6.1 project environment.")
endif()

if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32p4" CACHE STRING "Target chip")
endif()
if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "This board configuration targets esp32p4, not ${IDF_TARGET}.")
endif()

set(ENV{ESP_IDF_VERSION} "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
set(ENV{IDF_VERSION} "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/app/nes"
    "${CMAKE_CURRENT_LIST_DIR}/components/board_support"
)
# Профиль зафиксирован в исходниках; продукт не выбирает экспериментальные ветви.
set(NES_HOSTED_EXPLICIT_INIT ON)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/NesC6Audio.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/P4CpuClock.cmake")
nes_p4_cpu_prepare_config()
# Формат include также используется расширением ESP-IDF для выбора проекта.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Сборка ограничена main и его полным транзитивным замыканием SDK.
idf_build_set_property(MINIMAL_BUILD ON)
set(PROJECT_VER "0.1-beta")
project(bitvyr_nes_p4c6)
nes_p4_cpu_apply()
nes_c6_audio_attach_hosted()

if(NES_HOSTED_EXPLICIT_INIT)
    # Hosted 2.13.1 starts its transport in a constructor. At that point P4
    # startup-stack memory is still reserved. Our Bluetooth task explicitly
    # starts the same public API after app_main, without changing SDK sources.
    idf_component_get_property(NES_HOSTED_LIB espressif__esp_hosted COMPONENT_LIB)
    get_target_property(NES_HOSTED_SOURCES ${NES_HOSTED_LIB} SOURCES)
    set(NES_HOSTED_DEFERRED_SOURCES "${NES_HOSTED_SOURCES}")
    list(FILTER NES_HOSTED_DEFERRED_SOURCES EXCLUDE REGEX "(^|/)port_esp_hosted_host_init[.]c$")
    list(LENGTH NES_HOSTED_SOURCES NES_HOSTED_SOURCE_COUNT)
    list(LENGTH NES_HOSTED_DEFERRED_SOURCES NES_HOSTED_DEFERRED_SOURCE_COUNT)
    math(EXPR NES_HOSTED_REMOVED "${NES_HOSTED_SOURCE_COUNT} - ${NES_HOSTED_DEFERRED_SOURCE_COUNT}")
    if(NOT NES_HOSTED_REMOVED EQUAL 1)
        message(FATAL_ERROR "Review Hosted constructor selection after changing its pinned version.")
    endif()
    set_property(TARGET ${NES_HOSTED_LIB} PROPERTY SOURCES "${NES_HOSTED_DEFERRED_SOURCES}")
    idf_component_get_property(NES_BOARD_LIB board_support COMPONENT_LIB)
    target_compile_definitions(${NES_BOARD_LIB} PRIVATE NES_HOSTED_EXPLICIT_INIT=1)
endif()

# On Windows, Picolibc's default relative sysroot can push multilib C++
# header paths past MAX_PATH in a nested SDK installation. Keep the chosen
# install location and pass its canonical prefix instead of modifying the SDK.
if(CMAKE_HOST_WIN32 AND CONFIG_LIBC_PICOLIBC
   AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND COMMAND idf_toolchain_add_flags)
    get_filename_component(NES_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(NES_COMPILER_ROOT "${NES_COMPILER_BIN}/.." REALPATH)
    file(TO_CMAKE_PATH "${NES_COMPILER_ROOT}" NES_COMPILER_ROOT)
    # ESP-IDF also passes cflags/cxxflags to the linker driver. Adding the
    # prefix to ldflags too would expand Picolibc's specs twice.
    idf_toolchain_add_flags(
        COMPILE_OPTIONS "--picolibc-prefix=${NES_COMPILER_ROOT}"
    )
endif()
