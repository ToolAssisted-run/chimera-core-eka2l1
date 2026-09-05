#include "machine.h"
#include "host-ui.h"

#include <common/path.h>
#include <config/app_settings.h>
#include <config/config.h>
#include <kernel/timing.h>
#include <package/manager.h>
#include <system/devices.h>
#include <system/epoc.h>

#include <algorithm>

namespace chimera {
    machine::machine(const machine_options &options)
        : options_(options)
        , clock_(options.epoch_us, options.instructions_per_second)
        , slice_instructions_(0) {
        // The clock goes in before anything can read it: the emulator's own
        // teletimer is built on this source, and it is sampled the moment the
        // nanokernel timer starts.
        if (!options_.host_clock) {
            clock_.install();
        }

        conf_ = std::make_unique<eka2l1::config::state>();
        conf_->storage = options_.storage;

        // The interpreter is the reference backend, the scheduler must return
        // rather than sleep when nothing is runnable (this loop owns the
        // waiting), and neither the bitmap compressor nor the app scan may run
        // on a thread of its own.
        conf_->cpu_backend = "dyncom";
        conf_->cpu_load_save = false;
        conf_->fbs_enable_compression_queue = false;
        conf_->single_thread_app_scan = true;

        settings_ = std::make_unique<eka2l1::config::app_settings>(conf_.get());

        eka2l1::system_create_components comp;
        comp.graphics_ = nullptr;
        comp.audio_ = nullptr;
        comp.conf_ = conf_.get();
        comp.settings_ = settings_.get();
        comp.cache_root_ = options_.storage;

        sys_ = std::make_unique<eka2l1::system>(comp);

        sys_->set_cpu_ticks_callback([this](const std::uint32_t instructions) {
            slice_instructions_ += instructions;
            clock_.spend_instructions(instructions);
        });
    }

    machine::~machine() {
        sys_.reset();
        virtual_clock::uninstall();
    }

    void machine::startup() {
        sys_->startup();

        // Nobody else may move this machine's time. The nanokernel timer would
        // otherwise start a thread that sleeps on the host clock and fires the
        // kernel's timers off it.
        sys_->get_ntimer()->set_driven(true);

        // reset() is what starts the timer's own measurement - and, in driven
        // mode, all it does. Without it the timer has never been told when
        // "now" began and reads the same instant forever, so no event it holds
        // ever comes due. set_device() calls it again, which costs nothing.
        sys_->get_ntimer()->reset();
    }

    std::size_t machine::device_count() const {
        return sys_->get_device_manager()->total();
    }

    bool machine::set_device(const std::size_t index) {
        return sys_->set_device(static_cast<std::uint8_t>(index));
    }

    void machine::boot() {
        if (options_.in_memory_drives) {
            // Drive Z is the ROM's, always: the ROM carries its own filesystem
            // and the ROM filesystem is the only one that will take a volume of
            // rom media. The writable drives are the caller's.
            sys_->mount(drive_z, drive_media::rom,
                eka2l1::add_path(options_.storage, "/drives/z/"),
                io_attrib_internal | io_attrib_write_protected);
        } else {
            mount_host_drives();
        }

        sys_->initialize_user_parties();

        eka2l1::manager::packages *packages = sys_->get_packages();
        packages->load_registries();
        packages->migrate_legacy_registries();
    }

    void machine::mount_host_drives() {
        // Drive Z is the ROM's own filesystem and can only be mounted once the
        // device has been set, because setting the device is what loads the ROM.
        sys_->mount(drive_c, drive_media::physical,
            eka2l1::add_path(options_.storage, "/drives/c/"), io_attrib_internal);
        sys_->mount(drive_d, drive_media::physical,
            eka2l1::add_path(options_.storage, "/drives/d/"), io_attrib_internal);
        sys_->mount(drive_e, drive_media::physical,
            eka2l1::add_path(options_.storage, "/drives/e/"), io_attrib_removeable);
        sys_->mount(drive_z, drive_media::rom,
            eka2l1::add_path(options_.storage, "/drives/z/"),
            io_attrib_internal | io_attrib_write_protected);
    }

    std::uint64_t machine::run_for_us(const std::uint64_t us) {
        const std::uint64_t target = clock_.now_us() + us;
        std::uint64_t loops = 0;

        eka2l1::ntimer *timing = sys_->get_ntimer();

        // Without a device there is no memory model, and with no memory model
        // there is nothing to schedule: the emulator's loop would switch to a
        // thread that cannot exist. Time still passes for such a machine, and
        // its timers still come due, which is exactly what an empty machine is
        // useful for testing.
        const bool runnable = (sys_->get_memory_system() != nullptr);

        while (clock_.now_us() < target) {
            slice_instructions_ = 0;

            if (runnable) {
                sys_->loop();
                loops++;
            }

            // Dialogs raised during the slice are answered here, outside the
            // kernel lock the caller was holding.
            pump_host_ui();

            // Fire whatever the instructions just bought, and learn when the
            // next deadline is.
            const std::optional<std::uint64_t> next = timing->advance();

            if (slice_instructions_ != 0) {
                continue;
            }

            // Nothing ran: every thread is waiting on something. Time still
            // passes, so buy it outright - up to the next deadline, and never
            // past the end of the slice we were asked for.
            const std::uint64_t remaining = target - clock_.now_us();
            const std::uint64_t jump = next.has_value() ? std::max<std::uint64_t>(next.value(), 1) : remaining;

            clock_.advance_us(std::min(jump, remaining));
        }

        return loops;
    }
}
