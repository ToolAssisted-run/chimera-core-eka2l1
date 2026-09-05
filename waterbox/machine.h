#pragma once

#include "vclock.h"

#include <cstdint>
#include <memory>
#include <string>

namespace eka2l1 {
    class system;

    namespace config {
        class app_settings;
        struct state;
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

        // How many devices the storage holds, and whether one could be set.
        std::size_t device_count() const;
        bool set_device(const std::size_t index);

        // Everything the frontend's stage two does that a machine needs: the
        // drives mounted, the user-side servers created, the package registry
        // read. Call it after a device is set and before stepping. With
        // in_memory_drives the mounting is the caller's - it has already been
        // done, against a filesystem the caller owns.
        void boot();

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

        // Instructions the emulator reported during the loop() call in flight.
        std::uint32_t slice_instructions_;
    };
}
