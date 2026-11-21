# Общие операции с целями ESP-IDF после project().
include_guard(GLOBAL)

#[[ Убирает автозапуск Hosted. Задача Bluetooth запускает тот же драйвер после
app_main, когда память стартового стека уже доступна для выделения. ]]
function(nes_defer_hosted_init)
    idf_component_get_property(hosted espressif__esp_hosted COMPONENT_LIB)
    get_target_property(sources ${hosted} SOURCES)
    set(kept "${sources}")
    list(FILTER kept EXCLUDE REGEX "(^|/)port_esp_hosted_host_init[.]c$")
    list(LENGTH sources before)
    list(LENGTH kept after)
    math(EXPR removed "${before} - ${after}")
    if(NOT removed EQUAL 1)
        message(FATAL_ERROR "Review Hosted constructor selection after changing its pinned version.")
    endif()
    set_property(TARGET ${hosted} PROPERTY SOURCES "${kept}")
endfunction()

#[[ Задаёт короткий путь к sysroot Picolibc для Windows. ESP-IDF сам передаёт
флаг компилятора на этап линковки, поэтому второй флаг там не требуется. ]]
function(nes_picolibc_windows_prefix)
    if(CMAKE_HOST_WIN32 AND CONFIG_LIBC_PICOLIBC
       AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       AND COMMAND idf_toolchain_add_flags)
        get_filename_component(compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        get_filename_component(compiler_root "${compiler_bin}/.." REALPATH)
        file(TO_CMAKE_PATH "${compiler_root}" compiler_root)
        idf_toolchain_add_flags(COMPILE_OPTIONS "--picolibc-prefix=${compiler_root}")
    endif()
endfunction()
