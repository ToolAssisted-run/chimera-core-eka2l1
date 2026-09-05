// The native reference harness: it builds the emulator the way the Qt
// frontend's stage_one does, minus Qt and minus every host device, steps it,
// and reports what the machine did. No graphics driver, no audio driver, no
// window.
//
//   run-native --data <storage root> [--frames N] [--fps R] [--sleep-ms M]
//
// Everything it prints is derived from the machine, never from the host clock,
// which is what makes two runs comparable. --sleep-ms is the proof rather than
// a convenience: it stalls the host between frames, and a machine whose time
// still came from the wall would report a different timeline.
#include "machine.h"
#include "memfs.h"

#include <common/log.h>

extern "C" int chimera_egl_make_context(char *err, int errlen);
extern "C" void *chimera_egl_proc(const char *name);

#include <common/cvt.h>
#include <kernel/kernel.h>
#include <kernel/timing.h>
#include <services/applist/applist.h>
#include <common/path.h>
#include <system/consts.h>
#include <vfs/vfs.h>
#include <utils/apacmd.h>
#include <system/devices.h>
#include <system/epoc.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {
    // A picture, for looking at rather than for the gate.
    bool write_tga(const char *path, const std::uint32_t *bgra, const int w, const int h) {
        std::FILE *f = std::fopen(path, "wb");

        if (!f) {
            return false;
        }

        std::uint8_t header[18] = { 0 };

        header[2] = 2;
        header[12] = w & 0xff;
        header[13] = (w >> 8) & 0xff;
        header[14] = h & 0xff;
        header[15] = (h >> 8) & 0xff;
        header[16] = 32;
        header[17] = 0x20;

        std::fwrite(header, 1, sizeof(header), f);
        std::fwrite(bgra, 4, static_cast<std::size_t>(w) * h, f);
        std::fclose(f);

        return true;
    }

    // Drives served from memory have no host path, so they cannot be mounted
    // through mount_physical_path and nothing announces them. The application
    // list only scans a drive it has been told about.
    void announce_memory_drives(eka2l1::io_system *io) {
        for (const drive_number drv : { drive_c, drive_d, drive_e }) {
            io->announce_drive(drv, eka2l1::drive_action_mount);
        }
    }
}

int main(int argc, char **argv) {
    chimera::machine_options options;
    options.storage = "data";

    int frames = 0;
    int fps = 60;
    int sleep_ms = 0;
    int timer_period_us = 0;
    bool list_apps = false;
    std::string run_path;
    std::string probe_path;
    std::string rom_only_path;
    std::string install_path;
    std::string card_path;
    int press_button = -1;
    bool print_rom_path = false;
    bool verbose = false;
    bool gpu = false;
    std::string screen_out;

    for (int i = 1; i < argc; i++) {
        const bool has_value = (i + 1 < argc);

        if ((std::strcmp(argv[i], "--data") == 0) && has_value) {
            options.storage = argv[++i];
        } else if ((std::strcmp(argv[i], "--frames") == 0) && has_value) {
            frames = std::atoi(argv[++i]);
        } else if ((std::strcmp(argv[i], "--fps") == 0) && has_value) {
            fps = std::atoi(argv[++i]);
        } else if ((std::strcmp(argv[i], "--sleep-ms") == 0) && has_value) {
            sleep_ms = std::atoi(argv[++i]);
        } else if ((std::strcmp(argv[i], "--timer-us") == 0) && has_value) {
            timer_period_us = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--host-clock") == 0) {
            options.host_clock = true;
        } else if ((std::strcmp(argv[i], "--probe") == 0) && has_value) {
            probe_path = argv[++i];
        } else if ((std::strcmp(argv[i], "--press") == 0) && has_value) {
            press_button = std::atoi(argv[++i]);
        } else if ((std::strcmp(argv[i], "--card") == 0) && has_value) {
            card_path = argv[++i];
        } else if ((std::strcmp(argv[i], "--install") == 0) && has_value) {
            install_path = argv[++i];
        } else if ((std::strcmp(argv[i], "--rom-only") == 0) && has_value) {
            rom_only_path = argv[++i];
        } else if (std::strcmp(argv[i], "--gpu") == 0) {
            gpu = true;
        } else if ((std::strcmp(argv[i], "--screen-out") == 0) && has_value) {
            screen_out = argv[++i];
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--log-svc") == 0) {
            verbose = true;
            options.log_syscalls = true;
        } else if (std::strcmp(argv[i], "--print-rom-path") == 0) {
            print_rom_path = true;
        } else if (std::strcmp(argv[i], "--list-apps") == 0) {
            list_apps = true;
        } else if ((std::strcmp(argv[i], "--run") == 0) && has_value) {
            run_path = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (fps <= 0) {
        std::fprintf(stderr, "--fps must be positive\n");
        return 2;
    }

    eka2l1::log::setup_log(nullptr);

    // What the emulator has to say. Off by default: the gate compares output,
    // and the log is a running commentary with paths in it.
    if (verbose) {
        eka2l1::log::toggle_console();
    }

    // With a ROM and nothing else, the writable drives come from the machine's
    // own memory - the same filesystem the sandbox uses, exercised where it can
    // be debugged - and drive Z comes from the ROM.
    options.in_memory_drives = !rom_only_path.empty();

    chimera::machine machine(options);

    std::shared_ptr<chimera::memory_file_system> drives;

    if (!rom_only_path.empty()) {
        drives = std::make_shared<chimera::memory_file_system>();
    }


    std::printf("storage: %s\n", options.storage.c_str());
    std::printf("clock: %s\n", options.host_clock ? "the host's" : "virtual");

    // startup() needs no device: it builds the timer, the physical filesystem,
    // the exclusive monitor, the CPU core and the kernel. Whether there is a
    // device to boot afterwards is a separate question, and the answer to it is
    // the user's to supply.
    machine.startup();

    if (gpu) {
        char err[256] = { 0 };

        if (!chimera_egl_make_context(err, sizeof(err))) {
            std::fprintf(stderr, "no OpenGL context: %s\n", err);
            return 1;
        }

        machine.start_graphics(chimera_egl_proc);
        std::printf("gpu: %s\n", machine.has_graphics() ? "on" : "refused");
    }

    if (drives) {
        eka2l1::file_system_inst as_instance = drives;
        machine.sys()->get_io_system()->add_filesystem(as_instance);

        if (!machine.add_device_from_rom(rom_only_path)) {
            std::fprintf(stderr, "%s does not say which device it is\n", rom_only_path.c_str());
            return 1;
        }

        drives->mount_empty(drive_c, drive_media::physical, io_attrib_internal);
        drives->mount_empty(drive_d, drive_media::physical, io_attrib_internal);
        drives->mount_empty(drive_e, drive_media::physical, io_attrib_removeable);

        const eka2l1::device &detected = machine.sys()->get_device_manager()->get_devices()[0];

        std::printf("rom: %s %s (%s) epocver=%d\n", detected.manufacturer.c_str(),
            detected.model.c_str(), detected.firmware_code.c_str(), static_cast<int>(detected.ver));
    }

    const std::size_t devices = machine.device_count();

    std::printf("devices: %zu\n", devices);

    for (std::size_t i = 0; i < devices; i++) {
        const eka2l1::device &dvc = machine.sys()->get_device_manager()->get_devices()[i];
        std::printf("device %zu: %s %s (%s) epocver=%d\n", i, dvc.manufacturer.c_str(),
            dvc.model.c_str(), dvc.firmware_code.c_str(), static_cast<int>(dvc.ver));
    }

    static const char *const backend_names[] = { "unicorn", "dynarmic", "12l1r", "dyncom" };
    const int backend = static_cast<int>(machine.sys()->get_cpu_executor_type());

    std::printf("startup: ok\n");
    std::printf("cpu resolved: %s\n", (backend >= 0 && backend < 4) ? backend_names[backend] : "unknown");
    std::printf("kernel: %s\n", machine.sys()->get_kernel_system() ? "up" : "absent");
    std::printf("timer: %s driven\n", machine.sys()->get_ntimer()->driven() ? "up," : "up, NOT");

    if (devices > 0) {
        const bool set = machine.set_device(0);
        std::printf("device 0: %s\n", set ? "set" : "refused");

        if (set) {
            machine.boot();

            if (drives) {
                // The drives were mounted on a filesystem of their own, which
                // nothing else could have been told about.
                announce_memory_drives(machine.sys()->get_io_system());
            }

            std::printf("boot: ok\n");

        }
    }

    // The memory model follows the device's Symbian version, so the MMU only
    // exists once a device has been set.
    std::printf("memory: %s\n", machine.sys()->get_memory_system() ? "up" : "absent, no device");

    if (!install_path.empty()) {
        // A package into the machine, the way the emulator's own installer
        // does it. Drive C is where a Symbian phone puts applications.
        const int result = machine.sys()->install_package(
            eka2l1::common::utf8_to_ucs2(install_path), drive_c);

        std::printf("install: %d\n", result);
    }

    if (!card_path.empty()) {
        if (drives) {
            // Straight from the archive onto the machine's own drive E.
            std::printf("card: %d files\n", machine.install_card(card_path));
        } else {
            // The emulator's own installer, which wants directories on a host.
            std::string found;

            const eka2l1::ngage_game_card_install_error result = machine.sys()->install_ngage_game_card(
                card_path, [&found](std::string name) { found = std::move(name); }, nullptr);

            std::printf("card: %d %s\n", static_cast<int>(result), found.c_str());
        }

        // The drive changed under the application list's feet.
        machine.sys()->get_io_system()->announce_drive(drive_e, eka2l1::drive_action_mount);
    }

    if (print_rom_path) {
        // The exact name the emulator will open the ROM under. A sandbox has
        // to mount it under that name and no other, and the construction is
        // upstream's, not ours to guess.
        for (std::size_t i = 0; i < devices; i++) {
            const eka2l1::device &dvc = machine.sys()->get_device_manager()->get_devices()[i];

            std::printf("rom path: %s\n", eka2l1::add_path(options.storage,
                eka2l1::add_path(eka2l1::preset::ROM_FOLDER_PATH,
                    eka2l1::add_path(eka2l1::common::lowercase_string(dvc.firmware_code),
                        eka2l1::preset::ROM_FILENAME))).c_str());
        }
    }

    if (!probe_path.empty()) {
        // Which drives the system believes it has, whichever filesystem is
        // holding them.
        std::string mounted;

        for (int drv = drive_a; drv <= drive_z; drv++) {
            if (machine.sys()->get_io_system()->get_drive_entry(static_cast<drive_number>(drv))) {
                mounted += static_cast<char>('a' + drv - drive_a);
            }
        }

        std::printf("drives: %s\n", mounted.empty() ? "(none)" : mounted.c_str());

        // What the machine's own filesystem says about a path, whichever
        // filesystem happens to be serving it.
        eka2l1::io_system *io = machine.sys()->get_io_system();
        const std::u16string wide = eka2l1::common::utf8_to_ucs2(probe_path);

        std::printf("probe %s: exist=%d dir=%d\n", probe_path.c_str(),
            io->exist(wide) ? 1 : 0, io->open_dir(wide) ? 1 : 0);

        if (std::unique_ptr<eka2l1::file> handle = io->open_file(wide, READ_MODE | BIN_MODE)) {
            std::uint8_t head[16] = { 0 };
            const std::size_t got = handle->read_file(head, 1, sizeof(head));

            std::printf("  open: size=%llu read=%zu bytes=%02x%02x%02x%02x\n",
                static_cast<unsigned long long>(handle->size()), got, head[0], head[1], head[2], head[3]);
        } else {
            std::printf("  open: refused\n");
        }

        // The same question the application scan asks: the directories under a
        // path, in the order the filesystem hands them back.
        std::unique_ptr<eka2l1::directory> listing = io->open_dir(wide, {}, io_attrib_include_dir);

        if (listing) {
            int shown = 0;

            while (auto entry = listing->get_next_entry()) {
                if (shown++ < 8) {
                    std::printf("  entry: %s (%s) full=%s\n", entry->name.c_str(),
                        entry->type == eka2l1::io_component_type::dir ? "dir" : "file",
                        entry->full_path.c_str());
                }
            }

            std::printf("  entries: %d\n", shown);
        }
    }

    if (list_apps || !run_path.empty()) {
        eka2l1::kernel_system *kern = machine.sys()->get_kernel_system();
        eka2l1::applist_server *applist = reinterpret_cast<eka2l1::applist_server *>(
            kern->get_by_name<eka2l1::service::server>(
                eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));

        if (!applist) {
            std::printf("apps: no application list server\n");
        } else if (list_apps) {
            std::vector<eka2l1::apa_app_registry> &regs = applist->get_registerations();

            for (auto &reg : regs) {
                std::printf("app 0x%08x: %s\n", reg.mandatory_info.uid,
                    eka2l1::common::ucs2_to_utf8(reg.mandatory_info.long_caption.to_std_string(nullptr)).c_str());
            }
        }

        if (!run_path.empty() && applist) {
            // An application is launched the way the machine's own launcher
            // would: through its registration, not by opening a file. On EKA1
            // the executable behind a registration is not even the thing on
            // disk that carries its name.
            const bool by_uid = (run_path.size() > 2) && (run_path.substr(0, 2) == "0x");
            bool started = false;

            if (by_uid) {
                const std::uint32_t uid = static_cast<std::uint32_t>(std::strtoul(run_path.c_str(), nullptr, 16));
                eka2l1::apa_app_registry *registry = applist->get_registration(uid);

                if (registry) {
                    eka2l1::epoc::apa::command_line cmdline;
                    cmdline.launch_cmd_ = eka2l1::epoc::apa::command_create;

                    started = applist->launch_app(*registry, cmdline, nullptr, nullptr);
                }
            } else {
                eka2l1::process_ptr process = kern->spawn_new_process(
                    eka2l1::common::utf8_to_ucs2(run_path), u"");

                if (process) {
                    process->run();
                    started = true;
                }
            }

            std::printf("run: %s %s\n", run_path.c_str(), started ? "started" : "refused");
        }
    }

    // A kernel timer of our own, if asked for. It is the only workload an
    // empty machine has: the nanokernel timer is what the whole port's notion
    // of time runs through, so a periodic event that fires the same number of
    // times, at the same emulated microseconds, no matter what the host is
    // doing, is the thing M1 has to prove.
    eka2l1::ntimer *timing = machine.sys()->get_ntimer();

    std::uint64_t fired = 0;
    std::uint64_t last_fire_us = 0;
    std::uint64_t lateness_us = 0;
    int timer_event = 0;

    if (timer_period_us > 0) {
        timer_event = timing->register_event("chimera:timer-check",
            [&](const std::uint64_t userdata, const int late) {
                (void)userdata;

                fired++;
                last_fire_us = timing->microseconds();
                lateness_us += static_cast<std::uint64_t>(late);

                timing->schedule_event(timer_period_us, timer_event, 0);
            });

        timing->schedule_event(timer_period_us, timer_event, 0);
    }

    const std::uint64_t slice_us = 1000000ull / static_cast<std::uint64_t>(fps);
    std::uint64_t loops = 0;

    for (int i = 0; i < frames; i++) {
        // A key held for a moment in the middle of the run: the shortest way
        // to ask whether a machine that has gone quiet is waiting for input.
        if (press_button >= 0) {
            if (i == frames / 3) {
                machine.set_button(press_button, true);
            } else if (i == frames / 3 + 6) {
                machine.set_button(press_button, false);
            }
        }

        loops += machine.run_for_us(slice_us);

        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }

    if (gpu) {
        // What the machine drew. The digest is over the pixels the window
        // server last composited, and the lit count is the shortest way to say
        // whether anything reached the screen at all.
        std::vector<std::uint32_t> screen;
        int width = 0;
        int height = 0;

        if (!machine.read_screen(screen, width, height)) {
            std::printf("screen: none\n");
        } else {
            std::uint64_t digest = 1469598103934665603ull;
            std::size_t lit = 0;

            for (const std::uint32_t pixel : screen) {
                digest ^= pixel;
                digest *= 1099511628211ull;

                if ((pixel & 0x00FFFFFF) != 0) {
                    lit++;
                }
            }

            std::printf("screen: %dx%d digest %016llx lit %zu\n", width, height,
                static_cast<unsigned long long>(digest), lit);

            if (!screen_out.empty()) {
                write_tga(screen_out.c_str(), screen.data(), width, height);
            }
        }
    }

    // How many applications the machine has, counted after everything the run
    // installed - which is where the sandbox counts them too.
    if (eka2l1::kernel_system *kern = machine.sys()->get_kernel_system()) {
        eka2l1::applist_server *applist = reinterpret_cast<eka2l1::applist_server *>(
            kern->get_by_name<eka2l1::service::server>(
                eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));

        if (applist) {
            std::printf("apps: %zu\n", applist->get_registerations().size());
        }
    }

    if (drives) {
        // How much of a filesystem the machine is holding, and how much of it
        // the machine wrote - the same two numbers the sandbox reports.
        std::printf("drive entries: %zu\n", drives->entry_count());
        std::printf("drive written: %zu bytes\n", drives->written_bytes());
    }

    std::printf("frames: %d at %d fps\n", frames, fps);
    std::printf("virtual us: %llu\n", static_cast<unsigned long long>(machine.clock().elapsed_us()));
    std::printf("instructions: %llu\n", static_cast<unsigned long long>(machine.clock().instructions()));
    std::printf("loops: %llu\n", static_cast<unsigned long long>(loops));

    if (timer_period_us > 0) {
        std::printf("timer fired: %llu every %d us\n", static_cast<unsigned long long>(fired), timer_period_us);
        std::printf("timer last: %llu us\n", static_cast<unsigned long long>(last_fire_us));
        std::printf("timer lateness: %llu us\n", static_cast<unsigned long long>(lateness_us));
    }

    // The machine must be the only thing running. EKA2L1 starts a timer thread
    // of its own by default, and this is where that would show.
    std::error_code ec;
    const std::filesystem::directory_iterator tasks("/proc/self/task", ec);

    if (!ec) {
        std::size_t threads = 0;
        std::string names;

        for (const auto &entry : tasks) {
            threads++;

            // The name says which one, which is the whole point when a thread
            // that should not exist turns up.
            std::FILE *comm = std::fopen((entry.path() / "comm").c_str(), "r");

            if (comm) {
                char name[64] = { 0 };

                if (std::fgets(name, sizeof(name), comm)) {
                    std::string trimmed(name);
                    trimmed.erase(trimmed.find_last_not_of(" \n\r\t") + 1);

                    if (!names.empty()) {
                        names += ", ";
                    }

                    names += trimmed;
                }

                std::fclose(comm);
            }
        }

        std::printf("host threads: %zu (%s)\n", threads, names.c_str());
    }

    std::printf("teardown: ok\n");

    return 0;
}
