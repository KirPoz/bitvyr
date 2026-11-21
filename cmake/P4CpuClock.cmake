#[[
@file P4CpuClock.cmake
@brief Выбирает согласованные CPU360/400 и локальную RTC-реализацию P4 rev<3.
@details Только configure-владелец меняет три CPU-ключа SDKCONFIG до project().
После project() ON использует копию закреплённого SDK6.1 в build, не меняя SDK,
ревизию, eFuse или напряжение. MEM/APB также меняются180/90→200/100МГц;
частоты Flash80/PSRAM200 и Core PUBLIC-макросы этот модуль не задаёт.
]]
include_guard(GLOBAL)
option(NES_P4_CPU_OVERCLOCK "Use the reviewed project-local P4 CPU400 clock path" ON)
set_property(CACHE NES_P4_CPU_OVERCLOCK PROPERTY TYPE BOOL)
set(_NES_P4_CPU_RTC_ORIGINAL_SHA "e38fdcfc3ec305354adb20695818126d6190a34dff6dc45ba939fd6d84d94753")
set(_NES_P4_CPU_RTC_PATCHED_SHA "864ac0e0f2c2f10a934af0ec68c8d67450f27fd302a610b92b0080713a038adb")

#[[
@brief Меняет единственную строку CPU-ключа, сохраняя остальные байты config.
@param content Исходное содержимое выбранного SDKCONFIG, без разбора остальных ключей.
@param key Имя одного из трёх управляемых CPU-ключей.
@param line Полная новая строка, включая явное «is not set» для выключенной choice.
@param output Имя выходной переменной вызывающего configure-владельца.
@details Дубли ключа считаются ошибкой. Отсутствующий ключ добавляется с прежним EOL.
]]
function(_nes_p4_cpu_config_line content key line output)
    set(_pattern "(^|\n)(# ${key} is not set|${key}=[^\r\n]*)")
    string(REGEX MATCHALL "${_pattern}" _matches "${content}")
    list(LENGTH _matches _count)
    if(_count GREATER 1)
        message(FATAL_ERROR "Duplicate CPU key in SDKCONFIG: ${key}")
    endif()
    if(_count EQUAL 1)
        string(REGEX REPLACE "${_pattern}" "\\1${line}" _updated "${content}")
    else()
        if(content MATCHES "\r\n")
            set(_eol "\r\n")
        else()
            set(_eol "\n")
        endif()
        set(_updated "${content}")
        if(NOT _updated STREQUAL "" AND NOT _updated MATCHES "\n$")
            string(APPEND _updated "${_eol}")
        endif()
        string(APPEND _updated "${line}${_eol}")
    endif()
    set(${output} "${_updated}" PARENT_SCOPE)
endfunction()

#[[
@brief Согласует существующий или новый SDKCONFIG с явной опцией до Kconfig IDF.
@details Defaults не переопределяют существующие choice, поэтому меняются ровно
три строки активного SDKCONFIG. Если файла нет, началом служат полные defaults
этого проекта. Другие ключи/байты сохраняются; при совпадении запись не выполняется.
SDKCONFIG передаётся в родительский scope, NVS и настройки устройства не затрагиваются.
]]
function(nes_p4_cpu_prepare_config)
    if(CMAKE_PROJECT_INCLUDE_BEFORE)
        message(FATAL_ERROR "Clear external CMAKE_PROJECT_INCLUDE_BEFORE before using the project CPU clock policy.")
    endif()
    if(NOT SDKCONFIG)
        set(SDKCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig")
    endif()
    get_filename_component(SDKCONFIG "${SDKCONFIG}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(SDKCONFIG "${SDKCONFIG}" PARENT_SCOPE)
    if(NOT EXISTS "${SDKCONFIG}")
        get_filename_component(_config_dir "${SDKCONFIG}" DIRECTORY)
        file(MAKE_DIRECTORY "${_config_dir}")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults" "${SDKCONFIG}" COPYONLY)
    endif()
    file(READ "${SDKCONFIG}" _before)
    set(_after "${_before}")
    if(NES_P4_CPU_OVERCLOCK)
        set(_mhz 400)
        set(_choice360 "# CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360 is not set")
        set(_choice400 "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400=y")
    else()
        set(_mhz 360)
        set(_choice360 "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y")
        set(_choice400 "# CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400 is not set")
    endif()
    _nes_p4_cpu_config_line("${_after}" CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360 "${_choice360}" _after)
    _nes_p4_cpu_config_line("${_after}" CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400 "${_choice400}" _after)
    _nes_p4_cpu_config_line("${_after}" CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=${_mhz}" _after)
    if(NOT _before STREQUAL _after)
        file(WRITE "${SDKCONFIG}.nes-cpu.tmp" "${_after}")
        file(RENAME "${SDKCONFIG}.nes-cpu.tmp" "${SDKCONFIG}")
    endif()
    message(STATUS "NES_P4_CPU_CLOCK requested=${_mhz}MHz SDKCONFIG=${SDKCONFIG}")
endfunction()

#[[
@brief Выполняет ровно одну закреплённую вставку в заимствованный текст SDK.
@param source Исходный текст копии; глобальный файл SDK никогда не записывается.
@param old Единственный ожидаемый фрагмент закреплённого SDK6.1.
@param replacement Фрагмент с дополнительной CPU400-веткой.
@param output Имя выходной переменной вызывающего configure-владельца.
@details Отсутствие или повтор фрагмента останавливает configure для ревью SDK.
]]
function(_nes_p4_cpu_replace_once source old replacement output)
    string(FIND "${source}" "${old}" _first)
    string(FIND "${source}" "${old}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Review the pinned SDK RTC source before enabling CPU400.")
    endif()
    string(REPLACE "${old}" "${replacement}" _updated "${source}")
    set(${output} "${_updated}" PARENT_SCOPE)
endfunction()

#[[
@brief Проверяет Kconfig-результат и после project() выбирает одну RTC source-копию.
@details OFF оставляет официальный SDK CPU360. ON/pre-v3 требует PM OFF и две
точные вставки в SDK6.1, проверяет оба полных SHA и сохраняет per-source свойства.
Нет новой задачи/буфера/поля Core: build/generated/p4-clock/rtc_clk.c принадлежит
configure и исчезает при Full Clean, затем воспроизводится только из исходников.
]]
function(nes_p4_cpu_apply)
    if(NES_P4_CPU_OVERCLOCK)
        set(_expected 400)
    else()
        set(_expected 360)
    endif()
    if(NOT "${CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ}" STREQUAL "${_expected}")
        message(FATAL_ERROR "CPU option/Kconfig mismatch: requested${_expected}, actual${CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ}.")
    endif()
    if(NOT NES_P4_CPU_OVERCLOCK OR NOT CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
        return()
    endif()
    if(CONFIG_PM_ENABLE)
        message(FATAL_ERROR "The reviewed pre-v3 CPU400 profile requires CONFIG_PM_ENABLE=n.")
    endif()
    set(_original "$ENV{IDF_PATH}/components/esp_hw_support/port/esp32p4/rtc_clk.c")
    get_filename_component(_original "${_original}" REALPATH)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_original}")
    file(SHA256 "${_original}" _original_sha)
    if(NOT _original_sha STREQUAL _NES_P4_CPU_RTC_ORIGINAL_SHA)
        message(FATAL_ERROR "CPU400 RTC source changed in SDK; review the project-local adaptation first.")
    endif()
    file(READ "${_original}" _source)
    set(_old1 [=[#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    switch (cpu_freq_mhz) {
    case 360:]=])
    set(_new1 [=[#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    switch (cpu_freq_mhz) {
    case 400:
        mem_divider = 2;
        apb_divider = 2;
        break;
    case 360:]=])
    set(_old2 [=[        source_freq_mhz = CLK_LL_PLL_360M_FREQ_MHZ;
        divider.integer = 1;
    } else {]=])
    set(_new2 [=[        source_freq_mhz = CLK_LL_PLL_360M_FREQ_MHZ;
        divider.integer = 1;
    } else if (freq_mhz == 400) {
        real_freq_mhz = freq_mhz;
        source = SOC_CPU_CLK_SRC_CPLL;
        source_freq_mhz = CLK_LL_PLL_400M_FREQ_MHZ;
        divider.integer = 1;
    } else {]=])
    _nes_p4_cpu_replace_once("${_source}" "${_old1}" "${_new1}" _patched)
    _nes_p4_cpu_replace_once("${_patched}" "${_old2}" "${_new2}" _patched)
    set(_copy "${CMAKE_BINARY_DIR}/generated/p4-clock/rtc_clk.c")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/p4-clock")
    set(_copy_sha "")
    if(EXISTS "${_copy}")
        file(SHA256 "${_copy}" _copy_sha)
    endif()
    if(NOT _copy_sha STREQUAL _NES_P4_CPU_RTC_PATCHED_SHA)
        # SDK-источник закреплён с LF; Windows-native EOL меняет полный SHA копии.
        file(CONFIGURE OUTPUT "${_copy}" CONTENT "${_patched}" @ONLY NEWLINE_STYLE LF)
    endif()
    file(SHA256 "${_copy}" _copy_sha)
    if(NOT _copy_sha STREQUAL _NES_P4_CPU_RTC_PATCHED_SHA)
        message(FATAL_ERROR "Generated RTC source is not the reviewed two-insertion implementation.")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_copy}")
    if(NOT TARGET __idf_esp_hw_support)
        message(FATAL_ERROR "CPU400 requires the actual esp_hw_support target after project().")
    endif()
    get_target_property(_source_dir __idf_esp_hw_support SOURCE_DIR)
    get_target_property(_sources __idf_esp_hw_support SOURCES)
    set(_updated_sources)
    set(_count 0)
    foreach(_item IN LISTS _sources)
        get_filename_component(_absolute "${_item}" ABSOLUTE BASE_DIR "${_source_dir}")
        if(_absolute STREQUAL _original)
            math(EXPR _count "${_count}+1")
            foreach(_property COMPILE_OPTIONS COMPILE_FLAGS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES LANGUAGE OBJECT_DEPENDS SKIP_UNITY_BUILD_INCLUSION)
                get_source_file_property(_value "${_item}" TARGET_DIRECTORY __idf_esp_hw_support ${_property})
                if(NOT _value STREQUAL "NOTFOUND")
                    set_source_files_properties("${_copy}" TARGET_DIRECTORY __idf_esp_hw_support PROPERTIES ${_property} "${_value}")
                endif()
            endforeach()
            list(APPEND _updated_sources "${_copy}")
        else()
            list(APPEND _updated_sources "${_item}")
        endif()
    endforeach()
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "Expected one SDK RTC source, found${_count}; review SDK source selection.")
    endif()
    set_property(TARGET __idf_esp_hw_support PROPERTY SOURCES "${_updated_sources}")
    message(STATUS "NES_P4_CPU_CLOCK source=${_copy} sha256=${_copy_sha} CPU=400 MEM=200 APB=100")
endfunction()
