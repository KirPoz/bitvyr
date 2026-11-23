# Переносимое ядро C++17. Прошивка и нативные тесты используют одинаковые настройки.
add_library(nes_core STATIC
    "Machine.cpp"
    "cpu/Cpu6502.cpp"
    "ppu/Ppu.cpp"
    "apu/Apu.cpp"
    "cartridge/Cartridge.cpp"
    "cartridge/Legacy160.cpp"
    "ppu/TimingPpu.cpp"
    "ppu/VideoRenderer.cpp"
    "MachineVideo.cpp"
    "apu/TimingApu.cpp"
    "apu/AudioRenderer.cpp"
    "MachineMmc5LocalReadFold.cpp"
    "cartridge/Mmc5.cpp"
    "cartridge/Sunsoft5BAudio.cpp"
    "cartridge/Vrc7Audio.cpp"
    "cartridge/Mmc5Audio.cpp"
    "cartridge/CartridgeMmc5.cpp"
    "MachineMmc5.cpp"
    "apu/ApuExpansion.cpp"
    "ppu/TimingMmc5Raster.cpp"
    "cpu/CpuQuietBlock.cpp"
    "ppu/PpuColourReplay.cpp"
    "ColourStream.cpp"
    "ppu/PpuRgbOutput.cpp"
    "ThinPpu.cpp"
    "ppu/ThinTimingPpu.cpp"
    "cpu/CpuPrgTail.cpp"
)
add_library(NES::Core ALIAS nes_core)
target_include_directories(nes_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/../../../../../cmake/NesCxxStandard.cmake")
nes_require_cxx17(nes_core PUBLIC)
# Эти два single-source headers также компилирует C6 recipe, их wire/layout прежние.
target_compile_definitions(nes_core PUBLIC NES_CORE_EXPANSION_DAC_PACKET=1 NES_CORE_APU_RENDERER_DMC_DEADLINES=1)
# Аппаратно выбранные P16 body-политики; public headers/layout и scalar tests прежние.
target_compile_definitions(nes_core PRIVATE
    NES_EXPERIMENTAL_GENERIC_AFFINE_QUIET=1 NES_EXPERIMENTAL_OAM_TERMINAL_MASK=1)
# Быстрое окно кода RAM используется только исполнителем последовательностей инструкций CPU.
set_property(SOURCE "cpu/CpuQuietBlock.cpp" APPEND PROPERTY
    COMPILE_DEFINITIONS NES_EXPERIMENTAL_CPU_RAM_CODE_WINDOW=1)
# Эти признаки меняют интерфейс Machine и должны совпадать у всех её потребителей.
target_compile_definitions(nes_core PUBLIC
    NES_EXPERIMENTAL_MMC5_INACTIVE_GRANT=1
    NES_EXPERIMENTAL_MMC5_ZERO_POLL_EXACT=1)
target_compile_definitions(nes_core PRIVATE
    NES_EXPERIMENTAL_MMC5_DUMMY_SPRITE_REUSE=1
    NES_EXPERIMENTAL_MMC5_REPLAY_VISIBLE=1
    NES_EXPERIMENTAL_RETAIN_DISCRETE_STATUS=1
    NES_EXPERIMENTAL_MMC5_GRANT_SINGLE_AUDIO_CHECK=1)
set_target_properties(nes_core PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 0)
set_source_files_properties(
    "MachineMmc5LocalReadFold.cpp"
    "cartridge/Mmc5.cpp"
    "cartridge/Sunsoft5BAudio.cpp"
    "cartridge/Vrc7Audio.cpp"
    "cartridge/Mmc5Audio.cpp"
    "cartridge/CartridgeMmc5.cpp"
    "MachineMmc5.cpp"
    "apu/ApuExpansion.cpp"
    "ppu/TimingMmc5Raster.cpp"
    "cpu/CpuQuietBlock.cpp"
    "ppu/PpuColourReplay.cpp"
    "ColourStream.cpp"
    "ppu/PpuRgbOutput.cpp"
    "ThinPpu.cpp"
    "ppu/ThinTimingPpu.cpp"
    "cpu/CpuPrgTail.cpp"
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
target_compile_options(nes_core PRIVATE -O3 -fjump-tables -ftree-switch-conversion)
