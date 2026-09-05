# The core's own top level. Upstream is added underneath it unmodified (bar the
# patch series), so one configure builds the emulator, its test suite and the
# glue in this directory; the native and the guest flavor differ only by the
# toolchain file.
cmake_minimum_required(VERSION 3.16)
project(chimera_eka2l1 CXX C)

set(CHIMERA_EKA2L1_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

add_subdirectory(${CHIMERA_EKA2L1_ROOT}/extern/eka2l1 eka2l1)

# The reference harness: the emulator built, questioned and torn down by an
# embedder, with no frontend and no host devices.
add_executable(run-native run-native.cpp host-ui.cpp)
target_link_libraries(run-native PRIVATE epoc drivers config common)
