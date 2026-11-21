# Прикладной координатор и единственный рабочий видеоконвейер P4.
idf_component_register(
    SRCS "NesApp.cpp" "runtime/FramePipeline.cpp" "console/BufferedUart.cpp"
         "storage/GameLibrary.cpp" "storage/ZipArchive.cpp" "settings/SettingsStore.cpp"
    INCLUDE_DIRS "."
    REQUIRES board_support log esp_system esp_hw_support esp_timer esp_driver_uart nvs_flash
    PRIV_REQUIRES vfs
    LDFRAGMENTS
        "${PROJECT_DIR}/cmake/linker/core-memory.lf"
        "${PROJECT_DIR}/cmake/linker/cartridge-expansion-flash.lf"
        "${PROJECT_DIR}/cmake/linker/apu-renderer-text.lf"
        "${PROJECT_DIR}/cmake/linker/guest-irq-flash.lf"
        "${PROJECT_DIR}/cmake/linker/mmc5-tail-slot-flash.lf"
        "${PROJECT_DIR}/cmake/linker/quiet-controls-flash.lf"
        "${PROJECT_DIR}/cmake/linker/mmc5-raster-plan-flash.lf"
        "${PROJECT_DIR}/cmake/linker/event-kernel-flash.lf"
        "${PROJECT_DIR}/cmake/linker/cold-lifecycle-flash.lf"
        "${PROJECT_DIR}/cmake/linker/fused-rgb-flash.lf"
        "${PROJECT_DIR}/cmake/linker/full-ppu-remainder-flash.lf"
        "${PROJECT_DIR}/cmake/linker/foreign-video-flash.lf"
        "${PROJECT_DIR}/cmake/linker/render-fetched-iram.lf"
        "${PROJECT_DIR}/cmake/linker/cpu-quiet-resident-rebalance.lf"
        "${PROJECT_DIR}/cmake/linker/joined-output-controls-flash.lf"
        "${PROJECT_DIR}/cmake/linker/frame-output-inactive-legacy-flash.lf"
        "${PROJECT_DIR}/cmake/linker/mmc5-helpers-flash.lf"
 )
add_subdirectory(core)
target_compile_options(nes_core PRIVATE -O3 -fjump-tables -ftree-switch-conversion)
target_link_libraries(${COMPONENT_LIB} PUBLIC NES::Core)
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_vfs_uart_get_vfs")
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/NesCxxStandard.cmake")
nes_require_cxx17(${COMPONENT_LIB} PUBLIC)
