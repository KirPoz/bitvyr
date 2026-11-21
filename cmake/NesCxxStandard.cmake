include_guard(GLOBAL)

# Единая политика собственных targets: C++17 без GNU-расширений.
# visibility сохраняет прежний PRIVATE/PUBLIC контракт каждого потребителя.
function(nes_require_cxx17 target visibility)
    target_compile_features(${target} ${visibility} cxx_std_17)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    )
    # ESP-IDF6.1 вставляет -std=gnu++26 после стандартного CMake-флага.
    # Заключительный target option нужен: одного CXX_STANDARD недостаточно.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-std=c++17>")
        # Статические .su нужны для проверки стека конкретной сборки ESP32-P4.
        # Флаг создаёт метаданные компилятора без добавления гостевых операций.
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fstack-usage>")
    endif()
endfunction()
