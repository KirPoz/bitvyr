cmake_minimum_required(VERSION 3.16)
project(nes_native_tests LANGUAGES CXX)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Один вход для поддерживаемых native-наборов. Отдельные проекты ниже также
# остаются самостоятельными; Core-конфигурация передаётся обычными -D опциями.
add_subdirectory(host)
add_subdirectory(frame)
add_subdirectory(storage)
add_subdirectory(settings)

