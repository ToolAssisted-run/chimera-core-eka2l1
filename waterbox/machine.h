#pragma once

#include "vclock.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eka2l1 {
    class system;

    namespace config {
        class app_settings;
        struct state;
    }

    namespace drivers {
        class graphics_driver;
    }
}

namespace chimera {
    struct machine_options {
        // Where the device dump, the drives and the registries live.
        std::string storage;

        // What the machine believes its processor to be. 484 MHz is what the
        // emulator tells Symbian through the HAL, so one instruction per cycle
        // is the honest default until a real workload says otherwise.
        std::uint64_t instructions_per_second = 484000000;

        // What the machine believes the date to be when it starts: microseconds
        // since 1/1/1970, fixed at 2010-01-01T00:00:00Z. A machine whose RTC
        // moved between runs could not be replayed.
        std::uint64_t epoch_us = 1262304000ull * 1000000ull;

        // Leave the machine on the host's clock. Nothing a core does should
        // ever set this: it exists so the gate can show that a machine reading
        // the wall really does answer differently when the wall is stalled, and
        // that the check for it has teeth.
        bool host_clock = false;

        // The drives come from the machine's own memory rather than from
        // directories on a host: the sandbox has neither. See memfs.h.
        bool in_memory_drives = false;

        // Draw. The context is the embedder's (gl-context.h) and the driver is
        // driven from this loop rather than from a thread of its own.
        bool graphics = false;
    };

    // The emulator, its clock, and the loop that drives both. Everything a core
    // and the native reference share lives here; what differs between them is
    // only how they are asked to step.
    class machine {
    public:
        explicit machine(const machine_options &options);
        ~machine();

        // Builds the kernel, the timer and the CPU. The MMU arrives with the
        // device, which is a separate question.
        void startup();

        // Works out which device a ROM is - from the ROM - and registers it.
        // The machine then has one device, and its ROM is that file wherever
        // it happens to be. False when the ROM says nothing recognisable.
        bool add_device_from_rom(const std::string &rom_path);

        // How many devices the storage holds, and whether one could be set.
        std::size_t device_count() const;
        bool set_device(const std::size_t index);

        // Everything the frontend's stage two does that a machine needs: the
        // drives mounted, the user-side servers created, the package registry
        // read. Call it after a device is set and before stepping. With
        // in_memory_drives the mounting is the caller's - it has already been
        // done, against a filesystem the caller owns.
        void boot();

        // Builds the graphics driver on a context the embedder has already
        // made current, and gives it to the machine. Call after startup() and
        // before the device is set: the window server asks for a driver as it
        // is created.
        void start_graphics(void *(*loader)(const char *name));

        bool has_graphics() const {
            return gdriver_ != nullptr;
        }

        // The screen as the window server last composited it. False when the
        // machine has no screen yet - before a device, or before the window
        // server has made one.
        bool read_screen(std::vector<std::uint32_t> &out, int &width, int &height);

        // Runs the machine until its own clock has moved `us` forward. Returns
        // the number of times the emulator's loop was entered, which is a
        // property of the machine and not of the host.
        std::uint64_t run_for_us(const std::uint64_t us);

        eka2l1::system *sys() {
            return sys_.get();
        }

        const virtual_clock &clock() const {
            return clock_;
        }

    private:
        void mount_host_drives();

        machine_options options_;
        virtual_clock clock_;

        std::unique_ptr<eka2l1::config::state> conf_;
        std::unique_ptr<eka2l1::config::app_settings> settings_;
        std::unique_ptr<eka2l1::system> sys_;
        std::shared_ptr<eka2l1::drivers::graphics_driver> gdriver_;

        // Instructions the emulator reported during the loop() call in flight.
        std::uint32_t slice_instructions_;
    };
}
