idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "." REQUIRES nes)
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/NesCxxStandard.cmake")
nes_require_cxx17(${COMPONENT_LIB} PRIVATE)

# Только ранний startup адаптер P4; SDK, частоты и защита памяти не изменяются.
if(CONFIG_IDF_TARGET_ESP32P4)
    target_sources(${COMPONENT_LIB} PRIVATE "P4CacheHandoff.cpp")
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=spi_flash_enable_high_performance_mode")
endif()

