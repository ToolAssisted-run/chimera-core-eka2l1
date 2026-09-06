# The core's own top level. Upstream is added underneath it unmodified (bar the
# patch series), so one configure builds the emulator, its test suite and the
# glue in this directory; the native and the guest flavor differ only by the
# toolchain file.
cmake_minimum_required(VERSION 3.16)
project(chimera_eka2l1 CXX C)

set(CHIMERA_EKA2L1_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

add_subdirectory(${CHIMERA_EKA2L1_ROOT}/extern/eka2l1 eka2l1)

# The files the emulator expects to find beside itself - its patch libraries
# and its shaders - carried inside the binary: this machine has no folders for
# it to walk.
set(CHIMERA_EMBEDDED_FILES ${CMAKE_CURRENT_BINARY_DIR}/generated-embedded-files.cpp)

add_custom_command(
    OUTPUT ${CHIMERA_EMBEDDED_FILES}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CMAKE_CURRENT_LIST_DIR}/gen-embedded-files.py
            ${CHIMERA_EKA2L1_ROOT}/extern/eka2l1/src/patch
            ${CHIMERA_EKA2L1_ROOT}/extern/eka2l1/src/emu/drivers/resources/gl
            ${CHIMERA_EMBEDDED_FILES}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/gen-embedded-files.py
    COMMENT "Collecting the files the emulator would look for beside itself")

# The reference harness: the emulator built, questioned and torn down by an
# embedder, with no frontend and no host devices.
add_executable(run-native run-native.cpp host-ui.cpp machine.cpp vclock.cpp memfs.cpp gl-context.cpp gl-egl.c input.cpp audio.cpp null-graphics.cpp ${CHIMERA_EMBEDDED_FILES})
target_include_directories(run-native PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(run-native PRIVATE epoc epockern epocpkg epocservs epocutils drivers config common EGL)

# Provisioning: a Symbian ROM dump becomes an installed device under a storage
# root. It runs once, outside the machine.
add_executable(install-device install-device.cpp host-ui.cpp)
target_link_libraries(install-device PRIVATE epoc epockern drivers config common)
