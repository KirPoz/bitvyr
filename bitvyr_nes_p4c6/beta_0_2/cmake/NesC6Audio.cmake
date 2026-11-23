# Единственный product backend C6 Batch; capabilities/image admission выполняет runtime.
include_guard(GLOBAL)

#[[ @brief Подключает ровно один сохранённый SERIAL1 raw adapter; SERIAL0/BLE остаётся Hosted.
@details Inputs/version/SHA сверяются до generated replacement; SDK source не перезаписывается.
]]
function(nes_c6_audio_attach_app target)
    if(CMAKE_BUILD_EARLY_EXPANSION)
        return()
    endif()
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    target_include_directories(${target} PRIVATE "${root}/app/nes/core"
        "${root}/app/nes/audio/c6/protocol" "${root}/app/nes/audio/c6/transport"
        "${root}/app/nes/audio/c6/runtime" "${root}/app/nes/audio/c6/qualification")
    idf_component_get_property(hosted espressif__esp_hosted COMPONENT_LIB)
    get_target_property(hosted_includes ${hosted} INCLUDE_DIRECTORIES)
    target_include_directories(${target} PRIVATE ${hosted_includes})
    # Профиль HELLO по умолчанию закреплён для проверенного worker.
    # Для самостоятельно собранного C6 можно указать точный SHA его ELF.
    set(NES_C6_WORKER_SHA256 "" CACHE STRING "Expected ELF SHA-256 of the C6 worker")
    if(NOT NES_C6_WORKER_SHA256 STREQUAL "")
        string(LENGTH "${NES_C6_WORKER_SHA256}" hash_len)
        if(NOT hash_len EQUAL 64 OR NOT NES_C6_WORKER_SHA256 MATCHES "^[0-9a-fA-F]+$")
            message(FATAL_ERROR "NES_C6_WORKER_SHA256 must contain exactly 64 hex digits")
        endif()
        string(REGEX MATCHALL "[0-9a-fA-F][0-9a-fA-F]" hash_pairs "${NES_C6_WORKER_SHA256}")
        string(JOIN ",0x" hash_bytes ${hash_pairs})
        target_compile_definitions(${target} PRIVATE "NES_C6_WORKER_SHA256_BYTES=0x${hash_bytes}")
    endif()
    target_link_options(${target} INTERFACE "-Wl,--wrap=serial_ll_rx_handler")
endfunction()
#[[ @brief Сверяет pinned input SHA до generated replacement; несовпадение останавливает configure. ]]
function(nes_c6_audio_require_sha path expected)
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Review C6 audio Hosted input after version/source change: ${path}")
    endif()
endfunction()

#[[ @brief Проверяет fixed SDIO/BLE pins,clock,queues и stacks; не меняет SDK или NVS. ]]
function(nes_c6_audio_validate_sdkconfig)
    foreach(flag IN ITEMS CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS
            CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE CONFIG_ESP_HOSTED_SDIO_RX_DOUBLE_BUFFERING
            CONFIG_BT_ENABLED)
        if(NOT ${flag})
            message(FATAL_ERROR "C6_BATCH requires project sdkconfig ${flag}; no global SDK adaptation is performed")
        endif()
    endforeach()
    foreach(pair IN ITEMS "CONFIG_ESP_HOSTED_SDIO_SLOT=1" "CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH=4"
            "CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000" "CONFIG_ESP_HOSTED_SDIO_PIN_CMD=19"
            "CONFIG_ESP_HOSTED_SDIO_PIN_CLK=18" "CONFIG_ESP_HOSTED_SDIO_PIN_D0=14"
            "CONFIG_ESP_HOSTED_SDIO_PIN_D1=15" "CONFIG_ESP_HOSTED_SDIO_PIN_D2=16"
            "CONFIG_ESP_HOSTED_SDIO_PIN_D3=17" "CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54"
            "CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=8" "CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=8"
            "CONFIG_ESP_HOSTED_DFLT_TASK_STACK=3072" "CONFIG_ESP_HOSTED_RPC_TASK_STACK=4096")
        string(REPLACE "=" ";" fields "${pair}")
        list(GET fields 0 key)
        list(GET fields 1 expected)
        if(NOT DEFINED ${key} OR NOT "${${key}}" STREQUAL "${expected}")
            message(FATAL_ERROR "C6_BATCH requires project ${key}=${expected}; preserve the matched SDIO/pin/stack profile")
        endif()
    endforeach()
    if(CONFIG_ESP_HOSTED_SDIO_CHECKSUM)
        message(FATAL_ERROR "C6_BATCH pinned Hosted profile has SDIO checksum OFF; wire CRC remains mandatory")
    endif()
endfunction()

#[[ @brief Подключает ровно один проверенный Hosted driver overlay в build, сохраняя SERIAL0/BLE. ]]
function(nes_c6_audio_attach_hosted)
    nes_c6_audio_validate_sdkconfig()
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    idf_component_get_property(hosted espressif__esp_hosted COMPONENT_LIB)
    idf_component_get_property(hosted_dir espressif__esp_hosted COMPONENT_DIR)
    set(driver "${hosted_dir}/host/drivers/transport/sdio/sdio_drv.c")
    set(package "${root}/app/nes/audio/c6/hosted")
    nes_c6_audio_require_sha("${hosted_dir}/idf_component.yml" "5b1b06c03f1d4d635dd5767bf2f82103fe99afa0434b5a38b9e95e6aba0b1066")
    file(READ "${hosted_dir}/idf_component.yml" manifest)
    if(NOT manifest MATCHES "version: 2[.]13[.]1" OR NOT manifest MATCHES "commit_sha: 0e8d0e3f0c2723deaf15441200cb4d8c53fea230")
        message(FATAL_ERROR "C6_BATCH supports the pinned Hosted2.13.1 commit only")
    endif()
    nes_c6_audio_require_sha("${driver}" "eaa06fd0735811c8ab653c591b554a62a4aab3b9c0fd0a1b592e65866f2d76a4")
    nes_c6_audio_require_sha("${package}/p4_sdio_drv.c" "a18a0ae8ec4ba7e7ecb5ea56e6553108558057868a125708bc12f0c130a8a071")
    get_target_property(sources ${hosted} SOURCES)
    set(kept)
    set(removed 0)
    foreach(source IN LISTS sources)
        if(source MATCHES "(^|/)sdio_drv[.]c$")
            math(EXPR removed "${removed}+1")
        else()
            list(APPEND kept "${source}")
        endif()
    endforeach()
    if(NOT removed EQUAL 1)
        message(FATAL_ERROR "C6_BATCH must replace exactly one stock Hosted sdio_drv.c")
    endif()
    set(generated "${CMAKE_BINARY_DIR}/nes-c6-audio/hosted/sdio_drv.c")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/nes-c6-audio/hosted")
    configure_file("${package}/p4_sdio_drv.c" "${generated}" COPYONLY)
    set_property(TARGET ${hosted} PROPERTY SOURCES "${kept};${generated}")
    # Canonical .inc/.h включаются отсюда: их ../protocol и ../transport не раздваиваются.
    target_include_directories(${hosted} PRIVATE "${package}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${package}/HostExtension.inc" "${package}/RawRxBounds.h" "${package}/provenance.json")
endfunction()
