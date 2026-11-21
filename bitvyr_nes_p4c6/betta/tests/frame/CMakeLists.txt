# Actual Frame implementation, SDK allocator/time/logging только HOST mock.
add_executable(bitvyr_frame_ownership FrameOwnershipTests.cpp HostMemory.cpp
    ../../app/nes/runtime/frame/FrameVideoPipeline.cpp)
target_include_directories(bitvyr_frame_ownership PRIVATE . sdk_mock ../../app/nes
    ../../app/nes/runtime/frame ../../app/nes/core)
target_compile_definitions(bitvyr_frame_ownership PRIVATE NES_APP_C6_FRAME_VIDEO=1)
target_link_libraries(bitvyr_frame_ownership PRIVATE NES::Core)
nes_native_test_options(bitvyr_frame_ownership)
add_test(NAME frame_ownership COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_LIST_DIR}/check_reference.py" "$<TARGET_FILE:bitvyr_frame_ownership>"
    "${CMAKE_CURRENT_BINARY_DIR}/actual.bin")
set_tests_properties(frame_ownership PROPERTIES TIMEOUT 70)
