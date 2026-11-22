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

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/P4SdkCompatibility.cmake")
nes_defer_hosted_init()
nes_picolibc_windows_prefix()
