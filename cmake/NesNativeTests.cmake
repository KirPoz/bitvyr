include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/NesCxxStandard.cmake")

# Единые требования к собственным native-проверкам. Assertions остаются
# включёнными в Release: несколько fixtures выполняют проверяемый вызов в assert.
# SDK targets этот helper не подключают; политика не распространяется на vendor.
function(nes_native_test_options target)
    nes_require_cxx17(${target} PRIVATE)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /UNDEBUG)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    endif()
endfunction()
