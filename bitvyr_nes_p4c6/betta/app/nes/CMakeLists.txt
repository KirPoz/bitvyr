# Production dual-video и единый C6 audio: состав не переключается диагностическими options.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NesC6Audio.cmake")
idf_component_register(
    SRCS
        "NesApp.cpp"
        "storage/GameLibrary.cpp"
        "storage/ZipArchive.cpp"
        "settings/SettingsStore.cpp"
        "console/BufferedUart.cpp"
        "runtime/coupled/CoupledVideoPipeline.cpp"
        "runtime/bridge/ThinBridge.cpp"
        "runtime/frame/FrameVideoPipeline.cpp"
        "audio/c6/protocol/RawAsyncAdapter.cpp"
        "audio/c6/protocol/RawWire.cpp"
        "audio/c6/protocol/AudioPacketWire.cpp"
        "audio/c6/protocol/BatchClient.cpp"
        "audio/c6/transport/P4RawAsyncPort.cpp"
        "audio/c6/transport/P4RawRx.cpp"
        "audio/c6/transport/RawChannel.cpp"
    INCLUDE_DIRS "."
    REQUIRES board_support log esp_system esp_hw_support esp_timer esp_driver_uart nvs_flash
    PRIV_REQUIRES vfs espressif__esp_hosted
    LDFRAGMENTS
        "core-memory.lf"
        "../../cmake/linker/cartridge-expansion-flash.lf"
        "../../cmake/linker/c6-cold-dsp-tables-psram.lf"
        "apu-renderer-text.lf"
        "guest-irq-flash.lf"
        "mmc5-tail-slot-flash.lf"
        "mmc5-read-fold-flash.lf"
        "quiet-controls-flash.lf"
        "mmc5-raster-plan-flash.lf"
        "event-kernel-flash.lf"
        "runtime/placement/cpu-controls-flash.lf"
        "runtime/placement/rgb-controls-flash.lf"
        "../../cmake/c6-frame-video-cold-flash.lf"
        "../../cmake/c6-frame-video-fit-flash.lf"
        "../../cmake/mmc5-fetch-render-fused-flash.lf"
        "runtime/frame/foreign-video-flash.lf"
        "runtime/frame/render-fetched-iram.lf"
        "cpu-quiet-resident-rebalance.lf"
        "joined-output-controls-flash.lf"
        "mmc5-helpers-flash.lf"
)
nes_c6_audio_attach_app(${COMPONENT_LIB})
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_vfs_uart_get_vfs")
add_subdirectory(core)

# Выбранные P16 политики; исторические boot markers сохраняются без изменения ABI.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NES_APP_GENERIC_AFFINE_QUIET=1 NES_APP_OAM_TERMINAL_MASK=1)
target_link_libraries(${COMPONENT_LIB} PUBLIC NES::Core)

include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/NesCxxStandard.cmake")
nes_require_cxx17(${COMPONENT_LIB} PUBLIC)

# Явная memory policy C6: cold LUT освобождает HP-банк для единственного RGB source.
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
    message(FATAL_ERROR "C6 cold DSP tables require project external-BSS profile; use sdkconfig.defaults.")
endif()

# Выбранный CPU0 FM85 сохраняет прежний Core ABI и C6 wire.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NES_APP_CPU0_VRC7=1)
# Команда FETCH46 получает пакет PCM после DATA/Render.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NES_EXPERIMENTAL_BATCH_FETCH46=1)

# Main обслуживает C6 между порциями цветной обработки.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NES_EXPERIMENTAL_FRAME_COLOUR_PUMP=1)

# Третий блок PCM в PSRAM сохраняет остаток при частичной записи I2S.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NES_EXPERIMENTAL_C6_PCM_THREE_TICKETS=1)
