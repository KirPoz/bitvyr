# SDK entry и локальное согласование ICache при старте ESP32-P4.
idf_component_register(SRCS "main.cpp" "P4CacheHandoff.cpp" INCLUDE_DIRS "." REQUIRES nes)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/NesCxxStandard.cmake")
nes_require_cxx17(${COMPONENT_LIB} PRIVATE)
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=spi_flash_enable_high_performance_mode")
