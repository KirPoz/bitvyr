cmake_minimum_required(VERSION 3.16)
project(nes_core_host LANGUAGES CXX)
enable_testing()
add_compile_options(-include "${CMAKE_CURRENT_LIST_DIR}/BitvyrTestProfile.h")
include("${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/NesNativeTests.cmake")

# Тот же SDK-free автомат, который использует Display.cpp; Core не нужен.
add_executable(nes_game_present_poll_tests GamePresentPollTests.cpp)
target_include_directories(nes_game_present_poll_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../components/board_support/include")
nes_native_test_options(nes_game_present_poll_tests)
add_test(NAME game_present_poll COMMAND nes_game_present_poll_tests)

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../app/nes/core"
    "${CMAKE_CURRENT_BINARY_DIR}/nes_core"
)

nes_native_test_options(nes_core)

function(nes_test_target name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE NES::Core)
    nes_native_test_options(${name})
endfunction()
nes_test_target(nes_core_smoke CoreSmoke.cpp)
nes_test_target(nes_mmc5_wram_output_tests MachineMmc5WramOutputTests.cpp)
add_test(NAME mmc5_wram_output COMMAND nes_mmc5_wram_output_tests)
set_tests_properties(mmc5_wram_output PROPERTIES TIMEOUT 90)
nes_test_target(nes_machine_portable_carrier_tests MachinePortableCarrierTests.cpp)
add_test(NAME machine_portable_carrier COMMAND nes_machine_portable_carrier_tests
    --portable-carrier 1)
set_tests_properties(machine_portable_carrier PROPERTIES TIMEOUT 90)
nes_test_target(nes_machine_event_kernel_tests MachineEventKernelTests.cpp)
add_test(NAME machine_event_kernel COMMAND nes_machine_event_kernel_tests
    1 1
    --raster-plan 1
    --raster-interval 1
    --tail-slots 1)
set_tests_properties(machine_event_kernel PROPERTIES TIMEOUT 90)
# PRIVATE режим передаётся аргументом; headers и ABI у теста общие с Core.
nes_test_target(nes_mmc5_lazy_tests MachineMmc5LazyTests.cpp)
add_test(NAME mmc5_lazy COMMAND nes_mmc5_lazy_tests
    0)
set_tests_properties(mmc5_lazy PROPERTIES TIMEOUT 90)
nes_test_target(nes_machine_mmc5_read_fold_tests MachineMmc5ReadFoldTests.cpp)
add_test(NAME machine_mmc5_read_fold COMMAND nes_machine_mmc5_read_fold_tests
    0)
set_tests_properties(machine_mmc5_read_fold PROPERTIES TIMEOUT 90)
nes_test_target(nes_ppu_empty_sprite0_span_tests PpuEmptySprite0SpanTests.cpp)
add_test(NAME ppu_empty_sprite0_span COMMAND nes_ppu_empty_sprite0_span_tests)
set_tests_properties(ppu_empty_sprite0_span PROPERTIES TIMEOUT 90)
nes_test_target(nes_video_hot_state_tests VideoPacketHotStateTests.cpp)
add_test(NAME video_hot_state COMMAND nes_video_hot_state_tests)
nes_test_target(nes_video_segment_writer_tests VideoSegmentWriterTests.cpp)
add_test(NAME video_segment_writer COMMAND nes_video_segment_writer_tests)
nes_test_target(nes_run NesRun.cpp)
nes_test_target(nes_ppu_mask_timing_tests PpuMaskTimingTests.cpp)
add_test(NAME ppu_mask_timing COMMAND nes_ppu_mask_timing_tests)
nes_test_target(nes_menu_layout_tests MenuLayoutTests.cpp)
add_test(NAME menu_layout COMMAND nes_menu_layout_tests)
nes_test_target(nes_presentation_rate_tests PresentationRateTests.cpp)
add_test(NAME presentation_rate COMMAND nes_presentation_rate_tests)
nes_test_target(nes_game_controls_tests GameControlsTests.cpp)
add_test(NAME game_controls COMMAND nes_game_controls_tests)
nes_test_target(nes_pcm_stream_guard_tests PcmStreamGuardTests.cpp)
add_test(NAME pcm_stream_guard COMMAND nes_pcm_stream_guard_tests)
nes_test_target(nes_machine_timing_tests MachineTimingTests.cpp)
add_test(NAME machine_dma_timing COMMAND nes_machine_timing_tests)
nes_test_target(nes_machine_cart_ram_quantum_tests MachineCartRamQuantumTests.cpp)
add_test(NAME machine_cart_ram_quantum COMMAND nes_machine_cart_ram_quantum_tests
    1)
nes_test_target(nes_machine_quiet_mmio_prefilter_tests MachineQuietMmioPrefilterTests.cpp)
add_test(NAME machine_quiet_mmio_prefilter COMMAND nes_machine_quiet_mmio_prefilter_tests)
# Направленный portable DSP контракт независимо от выбранного PRIVATE режима.
nes_test_target(nes_apu_triangle_mean_tests ApuTriangleMeanTests.cpp)
add_test(NAME apu_triangle_mean COMMAND nes_apu_triangle_mean_tests
    1
    1)
set_tests_properties(apu_triangle_mean PROPERTIES TIMEOUT 30)

nes_test_target(nes_machine_frame_audio_tests MachineFrameAudioTests.cpp)
add_test(NAME machine_frame_audio COMMAND nes_machine_frame_audio_tests)
nes_test_target(nes_machine_mirroring_tests MachineMirroringTests.cpp)
add_test(NAME machine_mirroring COMMAND nes_machine_mirroring_tests)
add_test(NAME machine_guards COMMAND nes_core_smoke)
nes_test_target(nes_cartridge_tests CartridgeTests.cpp)
add_test(NAME cartridge COMMAND nes_cartridge_tests)
nes_test_target(mapper_expansion MapperExpansionTests.cpp)
add_test(NAME mapper_expansion COMMAND mapper_expansion)
nes_test_target(cartridge_expansion_audio CartridgeExpansionAudioTests.cpp)
add_test(NAME cartridge_expansion_audio COMMAND cartridge_expansion_audio)
nes_test_target(nes_test_roms TestRoms.cpp)
add_executable(nes_hid_tests
    ../../components/board_support/test/HostHidTests.cpp
    ../../components/board_support/input/HidGamepad.cpp
)
target_include_directories(nes_hid_tests PRIVATE ../../components/board_support/input)
nes_native_test_options(nes_hid_tests)
add_test(NAME hid_reports COMMAND nes_hid_tests)
nes_test_target(nes_touch_overlap_tests TouchOverlapTests.cpp)
add_test(NAME touch_overlap COMMAND nes_touch_overlap_tests)

# Проверки собственного MMC5 и его реальных PPU/audio контрактов.
foreach(mmc5_test Mmc5Tests Mmc5AudioTests)
    nes_test_target(${mmc5_test} ${mmc5_test}.cpp)
    if(mmc5_test STREQUAL "Mmc5AudioTests")
        # Ожидание передаётся данными: PRIVATE Core-макрос не меняет test headers.
        add_test(NAME ${mmc5_test} COMMAND ${mmc5_test}
            --selected-cache 1)
    else()
        add_test(NAME ${mmc5_test} COMMAND ${mmc5_test})
    endif()
endforeach()



# Независимый fullPPU/APU oracle и actual8192B wire для expansion-публикации.
nes_test_target(nes_mapper_expansion_frame_tests MapperExpansionFrameTests.cpp)
target_sources(nes_mapper_expansion_frame_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../app/nes/audio/c6/protocol/AudioPacketWire.cpp")
target_include_directories(nes_mapper_expansion_frame_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../app/nes")
target_compile_definitions(nes_mapper_expansion_frame_tests PRIVATE BITVYR_TEST_C6_AUDIO_WIRE=1)
add_test(NAME mapper_expansion_frame COMMAND nes_mapper_expansion_frame_tests)

# Реальный Output/RGB/PCM oracle и фактическая активация новых fastcap69/85.
nes_test_target(mapper_expansion_fastcap MapperExpansionFastcapTests.cpp)
add_test(NAME mapper_expansion_fastcap COMMAND mapper_expansion_fastcap)
# Exact32 audio сравнивается с frozen64 reference без product hooks.
add_executable(cartridge_expansion_audio_exact32 ExpansionAudioExact32Tests.cpp reference64/Sunsoft5BAudioWide.cpp reference64/Vrc7AudioWide.cpp)
target_link_libraries(cartridge_expansion_audio_exact32 PRIVATE NES::Core)
nes_native_test_options(cartridge_expansion_audio_exact32)
add_test(NAME cartridge_expansion_audio_exact32 COMMAND cartridge_expansion_audio_exact32)
set_tests_properties(cartridge_expansion_audio_exact32 PROPERTIES TIMEOUT 120)
# Настоящий stopPlayback исполняется без SDK через явные owner callbacks.
add_executable(stop_playback_lifecycle StopPlaybackLifecycleTests.cpp)
nes_native_test_options(stop_playback_lifecycle)
add_test(NAME stop_playback_lifecycle COMMAND stop_playback_lifecycle)
# WRAM69/85: реальные credits и независимые scalar CPU/RAM/bus/IRQ/PCM endpoint.
nes_test_target(mapper_expansion_wram_quiet MapperExpansionWramQuietTests.cpp)
add_test(NAME mapper_expansion_wram_quiet COMMAND mapper_expansion_wram_quiet)

# Main Frame wait продвигает C6, но join разрешает лишь настоящий done-token.
add_executable(frame_done_pump_wait FrameDoneWaitTests.cpp)
nes_native_test_options(frame_done_pump_wait)
add_test(NAME frame_done_pump_wait COMMAND frame_done_pump_wait)