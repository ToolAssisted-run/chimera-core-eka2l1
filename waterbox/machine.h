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

    /* Known 12-bit values into known colours: the check chimera#130 needed
     * and the gate did not have. */
    bool colour_check_12bpp(void);

    class audio_sink;

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

        // Log every kernel call the machine makes. Diagnostics only: it is
        // enormous, and it is the only way to see what a stalled application
        // last asked for.
        bool log_syscalls = false;

        // The rate the machine is asked to make sound at.
        std::uint32_t sample_rate = 44100;

        // Which ARM implementation runs the machine. "dyncom" is the
        // interpreter and the reference; "dynarmic" is the recompiler.
        std::string cpu_backend = "dyncom";
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

        // Gives the machine a driver that draws nowhere. A machine with no
        // driver at all does not merely draw nothing: a user-interface
        // application asking to be composed goes through a null pointer and
        // takes the machine with it. See null-graphics.h.
        void start_null_graphics();

        // Copies an N-Gage game card into the machine, onto drive E, straight
        // out of the archive it arrived in. The emulator's own card installer
        // unpacks to a directory on the host first and then copies with host
        // calls, which a machine whose drives are its own memory has no use
        // for. Returns the number of files written, 0 for "not a card".
        int install_card(const std::string &archive_path);

        // Starts an application the way the machine's own launcher would -
        // through its registration, not by opening a file. On EKA1 the
        // executable behind a registration is not even the thing on disk that
        // carries its name. Answers false when the machine has no such
        // registration, or refuses it.
        bool launch_app(std::uint32_t uid);

        // Remembers which applications the machine already had. Call before
        // installing anything: it is how the machine tells the project's own
        // application from the phone's.
        void remember_apps();

        // Starts the application the project brought, and answers which one.
        //
        // A Symbian machine with nothing running is a phone showing its menu,
        // and what a chimera project means by "the game" is the application
        // the card or the package put there - the registration that was not
        // there before. Zero when the project brought none, or when the
        // machine would not start it.
        std::uint32_t launch_installed_app();

        // The application this machine started, or zero.
        std::uint32_t launched_app() const {
            return launched_uid_;
        }

        // The game's own memory, one byte at a time.
        //
        // Symbian memory is not a block: the memory model gives every chunk its
        // own mapping, and a game's chunks do not exist until it has run. So
        // the machine offers an ADDRESS SPACE rather than a buffer - a chimera
        // bus - and resolves each address through the page tables of the
        // process it started, whichever thread happens to have run last.
        // Addresses nothing is mapped at read as zero and swallow writes.
        std::uint8_t peek_user(std::uint32_t addr);
        void poke_user(std::uint32_t addr, std::uint8_t value);

        // Puts one host file into the machine's own filesystem, at the path
        // given. The machine's drives have no host directory behind them, so
        // anything the machine is to find has to be written in through its own
        // filesystem like this. False when the file cannot be read or the
        // machine will not take it.
        bool put_file(const std::string &machine_path, const std::string &host_path);

        // Unpacks an N-Gage .blz card image into the machine, using the
        // Symbian application that knows how.
        //
        // A .blz is a compressed container with no public format; the machine
        // cannot read one and neither can this core. What CAN read one is
        // BLZinstapp, a Symbian application - so the machine installs it, puts
        // the .blz on its memory card where the application looks for it, runs
        // it, and works its two-key menu. Everything it unpacks lands in the
        // machine's own memory like any other install.
        //
        // Costs the machine the time the unpack takes, before the first frame
        // the caller ever asks for. Answers false when the application never
        // came up, never finished, or unpacked nothing.
        bool install_blz(const std::string &blz_path, const std::string &installer_path);

        // Installs a Symbian package into the machine, on drive C. The path
        // is a file the host mounted; what it writes goes into the machine's
        // own filesystem. Returns the emulator's own result code, 0 for
        // installed.
        int install_package(const std::string &path);

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

        // Gives the machine somewhere to put its sound. Call after startup().
        void start_audio();

        // A frame's worth of it, stereo, at the declared rate. Silence when
        // the machine has made none.
        void render_audio(std::int16_t *out, const std::size_t frames);

        // A key of the machine's keypad, held or let go. Levels, not events:
        // the caller says what is held this frame and the machine is told only
        // about the changes.
        void set_button(const int index, const bool held);

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

        // What launch_app() last started.
        std::uint32_t launched_uid_ = 0;

        // The applications the machine had before the project's own arrived.
        std::vector<std::uint32_t> apps_before_install_;

        // Whose address space the game's memory lives in, and the last page
        // looked up in it: a search walks addresses in order, so one cached
        // translation carries almost all of them.
        std::uint8_t *resolve_user(std::uint32_t addr);

        std::int32_t game_asid_ = -1;
        std::uint32_t cached_page_ = 0xFFFFFFFF;
        std::uint8_t *cached_host_ = nullptr;
        std::unique_ptr<eka2l1::system> sys_;
        std::shared_ptr<eka2l1::drivers::graphics_driver> gdriver_;
        std::shared_ptr<audio_sink> adriver_;

        // Instructions the emulator reported during the loop() call in flight.
        std::uint32_t slice_instructions_;

        // What each key was doing last time, so only changes are sent.
        std::vector<bool> buttons_;
    };
}
