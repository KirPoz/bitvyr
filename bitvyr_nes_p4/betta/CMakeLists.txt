cmake_minimum_required(VERSION 3.16)
# Параметры переносимого Core фиксированы; частота CPU остаётся выбором сборки.
if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "Select ESP-IDF 6.1 in the ESP-IDF extension or activate its terminal.")
endif()
include("$ENV{IDF_PATH}/tools/cmake/version.cmake")
if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 1)
    message(FATAL_ERROR "Bitvyr 0.1 requires the pinned ESP-IDF 6.1 toolchain.")
endif()
# Hosted Kconfig использует версию выбранного SDK из окружения configure-процесса.
set(ENV{ESP_IDF_VERSION} "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
set(ENV{IDF_VERSION} "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")
set(IDF_TARGET esp32p4 CACHE STRING "Waveshare ESP32-P4 target")
if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "This board requires esp32p4.")
endif()
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/app/nes" "${CMAKE_CURRENT_LIST_DIR}/components/board_support")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/P4CpuClock.cmake")
nes_p4_cpu_prepare_config()
# Формат include также используется расширением ESP-IDF для выбора проекта.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# IDF собирает транзитивную зависимость main вместо всех компонентов SDK.
idf_build_set_property(MINIMAL_BUILD ON)
set(PROJECT_VER "0.1")
project(bitvyr_nes_p4)
nes_p4_cpu_apply()

# Hosted 2.13.1 запускается из Bluetooth-задачи после app_main, когда доступен heap.
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

# Короткий sysroot обходит MAX_PATH без изменения установленного SDK.
if(CMAKE_HOST_WIN32 AND CONFIG_LIBC_PICOLIBC
   AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND COMMAND idf_toolchain_add_flags)
    get_filename_component(NES_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(NES_COMPILER_ROOT "${NES_COMPILER_BIN}/.." REALPATH)
    file(TO_CMAKE_PATH "${NES_COMPILER_ROOT}" NES_COMPILER_ROOT)
    idf_toolchain_add_flags(
        COMPILE_OPTIONS "--picolibc-prefix=${NES_COMPILER_ROOT}"
    )
endif()
