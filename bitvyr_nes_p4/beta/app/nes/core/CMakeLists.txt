# Переносимое C++17 ядро: один продуктовый профиль и отдельный test-only scalar oracle.
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/CoreProfile.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../../../../cmake/NesCxxStandard.cmake")

# Создаёт Core из тех же исходников; scalar используется только независимыми bus fixtures.
function(bitvyr_create_core target source_root scalar)
    set(sources
        Machine.cpp
        MachineStatusPoll.cpp
        cpu/Cpu6502.cpp
        ppu/Ppu.cpp
        apu/Apu.cpp
        cartridge/Cartridge.cpp
        cartridge/Legacy160.cpp
        cartridge/Mmc5.cpp
        cartridge/Sunsoft5BAudio.cpp
        cartridge/Vrc7Audio.cpp
        cartridge/Mmc5Audio.cpp
        cartridge/CartridgeMmc5.cpp
        MachineMmc5.cpp
        apu/ApuExpansion.cpp
    )
    if(NOT scalar)
        list(APPEND sources
            ppu/TimingPpu.cpp
            ppu/TimingMmc5Raster.cpp
            ppu/VideoRenderer.cpp
            MachineVideo.cpp
            cpu/CpuQuietBlock.cpp
            apu/TimingApu.cpp
            apu/AudioRenderer.cpp
            ppu/PpuColourReplay.cpp
            ColourStream.cpp
            ppu/PpuRgbOutput.cpp
            cpu/CpuPrgTail.cpp
        )
    else()
        # Scalar oracle сохраняет независимые bus и точный packet-APU контракт.
        list(APPEND sources apu/TimingApu.cpp apu/AudioRenderer.cpp)
    endif()
    list(TRANSFORM sources PREPEND "${source_root}/")
    add_library(${target} STATIC ${sources})
    target_include_directories(${target} PUBLIC "${source_root}")
    nes_require_cxx17(${target} PUBLIC)
    if(MSVC)
        target_compile_options(${target} PRIVATE /fp:strict)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -ffp-contract=off)
    endif()
    if(NOT scalar)
        target_compile_definitions(${target} PUBLIC ${BITVYR_CORE_PUBLIC_DEFINITIONS})
        target_compile_definitions(${target} PRIVATE ${BITVYR_CORE_PRIVATE_DEFINITIONS})
        set_target_properties(${target} PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 16)
        foreach(file IN ITEMS cartridge/Sunsoft5BAudio.cpp cartridge/Vrc7Audio.cpp cartridge/Mmc5.cpp cartridge/Mmc5Audio.cpp cartridge/CartridgeMmc5.cpp MachineMmc5.cpp apu/ApuExpansion.cpp MachineStatusPoll.cpp ppu/TimingMmc5Raster.cpp cpu/CpuQuietBlock.cpp cpu/CpuPrgTail.cpp ppu/PpuColourReplay.cpp ColourStream.cpp ppu/PpuRgbOutput.cpp)
            set_source_files_properties("${source_root}/${file}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
        endforeach()
    else()
        target_compile_definitions(${target} PUBLIC NES_CORE_SPLIT_APU=1 NES_CORE_EXPANSION_DAC_PACKET=1)
    endif()
endfunction()

bitvyr_create_core(nes_core "${CMAKE_CURRENT_SOURCE_DIR}" FALSE)
add_library(NES::Core ALIAS nes_core)
