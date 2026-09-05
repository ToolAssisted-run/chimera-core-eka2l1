# The adapter objects, compiled with the miniBox musl toolchain and the same
# flags CMake gave the emulator's libraries. build-guest.sh must have run
# first; build-core.sh links core.wbx from all of it.
#
# Usage: make -f guest.mk -j$(nproc)

ROOT   := ..
B      := $(ROOT)/build/guest
O      := obj-guest
MB     ?= $(HOME)/chimera/extern/tools/chimera-common-minibox
MBUILD := $(MB)/build/meson-cpp
SR     := $(MBUILD)/guest-sysroot
GCCVER := $(shell gcc -dumpfullversion)
EKA    := $(ROOT)/extern/eka2l1/src

WBFLAGS := -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector \
        -fno-pic -fno-pie -fcf-protection=none -O2 -std=gnu++20 \
        -DCHIMERA_CORE -DCHIMERA_GUEST -DDISABLE_LOGGING -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL \
        -Dthread_local= -D_Thread_local= -D__thread=
SPECS   := -specs $(SR)/lib/musl-gcc.specs
CXXINCS := -nostdinc++ -I$(SR)/include/c++/$(GCCVER) -I$(SR)/include/c++/$(GCCVER)/x86_64-linux-musl
MBINCS  := -I$(MB)/extern/emulibc -I$(MB)/source/guest/include -I$(MB)/extern/jsmn
EKAINCS := -I$(EKA)/emu/system/include -I$(EKA)/emu/kernel/include -I$(EKA)/emu/common/include \
        -I$(B)/eka2l1/src/emu/common/include -I$(EKA)/emu/config/include -I$(EKA)/emu/mem/include \
        -I$(EKA)/emu/drivers/include -I$(EKA)/emu/vfs/include -I$(EKA)/emu/utils/include \
        -I$(EKA)/emu/package/include -I$(EKA)/emu/services/include -I$(EKA)/emu/disasm/include \
        -I$(EKA)/emu/cpu/include -I$(EKA)/emu/bridge/include -I$(EKA)/emu/loader/include \
        -I$(EKA)/external/fmt/include -I$(EKA)/external/spdlog/include -I$(EKA)/external/yaml-cpp/include \
        -I$(EKA)/external/glm -I$(EKA)/external -I$(EKA)/external/xxHash -I$(EKA)/external/capstone/include -I$(EKA)/external/thread-pool/include

CXXFLAGS := $(WBFLAGS) $(MBINCS) $(EKAINCS) -I. $(CXXINCS)

OBJS := $(O)/wbx-entry.o $(O)/machine.o $(O)/vclock.o $(O)/memfs.o $(O)/host-ui.o $(O)/guest-syscalls.o

all: $(OBJS)

$(O)/%.o: %.cpp
	@mkdir -p $(O)
	g++ $(SPECS) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(O)

.PHONY: all clean
