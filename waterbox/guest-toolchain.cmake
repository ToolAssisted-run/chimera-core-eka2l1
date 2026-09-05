# The miniBox waterbox guest as a CMake toolchain: musl + static libstdc++,
# large code model, no PIC, no host libraries, no thread-local storage (the
# guest has no %fs, and the machine is single-threaded anyway). Exceptions stay
# on: EKA2L1 throws, and miniBox's cxxglue implements the unwinder's
# _dl_find_object for real.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED ENV{MINIBOX_SYSROOT})
  set(SR "$ENV{HOME}/chimera/extern/tools/chimera-common-minibox/build/meson-cpp/guest-sysroot")
else()
  set(SR "$ENV{MINIBOX_SYSROOT}")
endif()

execute_process(COMMAND gcc -dumpfullversion OUTPUT_VARIABLE GCCVER OUTPUT_STRIP_TRAILING_WHITESPACE)

set(WB "-specs=${SR}/lib/musl-gcc.specs -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -DCHIMERA_CORE -DCHIMERA_GUEST -DDISABLE_LOGGING -Dthread_local= -D_Thread_local= -D__thread=")
set(CMAKE_C_FLAGS_INIT "${WB}")
set(CMAKE_CXX_FLAGS_INIT "${WB} -nostdinc++ -I${SR}/include/c++/${GCCVER} -I${SR}/include/c++/${GCCVER}/x86_64-linux-musl")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -nostdlib++")

# Feature probes must not try to run guest binaries, and a full link needs the
# wbx entry glue that only build-core.sh has - compile and archive is the
# honest probe.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${SR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
