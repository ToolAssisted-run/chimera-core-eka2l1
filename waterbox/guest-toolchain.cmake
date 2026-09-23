# The miniBox waterbox guest as a CMake toolchain: musl + static libstdc++,
# large code model, no PIC, no host libraries, no thread-local storage (the
# guest has no %fs, and the machine is single-threaded anyway). Exceptions stay
# on: EKA2L1 throws, and miniBox's cxxglue implements the unwinder's
# _dl_find_object for real.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Where the guest sysroot is. MINIBOX_SYSROOT names it outright, MINIBOX_DIR
# names the miniBox checkout it lives in (build-guest.sh sets one of them from
# its -m), and only a developer's own tree is at $HOME/chimera.
if(DEFINED ENV{MINIBOX_SYSROOT})
  set(SR "$ENV{MINIBOX_SYSROOT}")
elseif(DEFINED ENV{MINIBOX_DIR})
  set(SR "$ENV{MINIBOX_DIR}/build/meson-cpp/guest-sysroot")
else()
  set(SR "$ENV{HOME}/chimera/extern/chimera-common-minibox/build/meson-cpp/guest-sysroot")
endif()

# A path that is not there produces "cannot read spec file" out of a CMake
# compiler probe, which says nothing about what is actually wrong.
if(NOT EXISTS "${SR}/lib/musl-gcc.specs")
  message(FATAL_ERROR "miniBox guest sysroot not at ${SR} - pass build-guest.sh -m <miniBox dir>")
endif()

execute_process(COMMAND gcc -dumpfullversion OUTPUT_VARIABLE GCCVER OUTPUT_STRIP_TRAILING_WHITESPACE)

# The compiler is PINNED to 13 where it exists, as CI's runner has it: GCC 14
# makes an implicit function declaration an error, and upstream's libarchive
# calls arc4random_buf, which the guest's musl 1.2.0 does not declare (its
# configure check links against a static archive, so it says yes). The tree
# built on this machine before 2026-09-23 got through only because it had been
# configured by hand with -Wno-implicit-function-declaration and friends -
# flags nothing in this repository sets - and a fresh configure failed.
find_program(GUEST_GCC NAMES gcc-13 gcc NO_CMAKE_FIND_ROOT_PATH)
find_program(GUEST_GXX NAMES g++-13 g++ NO_CMAKE_FIND_ROOT_PATH)
set(CMAKE_C_COMPILER "${GUEST_GCC}")
set(CMAKE_CXX_COMPILER "${GUEST_GXX}")

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
