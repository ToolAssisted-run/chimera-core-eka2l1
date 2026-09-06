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
#include <kernel/chunk.h>
#include <kernel/thread.h>
#include <kernel/timing.h>
#include <services/applist/applist.h>
#include <services/window/classes/winbase.h>
#include <services/window/classes/winuser.h>
#include <services/window/classes/wingroup.h>
#include <services/window/screen.h>
#include <services/window/window.h>
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

// The bus this harness reads, and the window of it worth comparing. See the
// block that digests them.
static constexpr std::uint32_t BUS_SIZE = 0x04000000;
static constexpr std::uint32_t BUS_BODY_FIRST = 0x00701000;
static constexpr std::uint32_t BUS_BODY_LAST = 0x02A00000;

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
    int press_raw = -1;
    bool print_rom_path = false;
    bool verbose = false;
    bool gpu = false;
    std::string screen_out;
    std::string bus_out;
    std::string extract_to;
    std::string put_spec;
    std::string verify_spec;
    std::string blz_path;
    std::string blz_installer;
    int press_at = -1;

    // Scripted keys: <frame>:<button> pairs, held for twenty frames each.
    std::vector<std::pair<int, int>> script;

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
        } else if ((std::strcmp(argv[i], "--press-raw") == 0) && has_value) {
            press_raw = static_cast<int>(std::strtol(argv[++i], nullptr, 0));
        } else if ((std::strcmp(argv[i], "--blz") == 0) && has_value) {
            blz_path = argv[++i];
        } else if ((std::strcmp(argv[i], "--blz-installer") == 0) && has_value) {
            blz_installer = argv[++i];
        } else if ((std::strcmp(argv[i], "--card") == 0) && has_value) {
            card_path = argv[++i];
        } else if ((std::strcmp(argv[i], "--install") == 0) && has_value) {
            install_path = argv[++i];
        } else if ((std::strcmp(argv[i], "--rom-only") == 0) && has_value) {
            rom_only_path = argv[++i];
        } else if ((std::strcmp(argv[i], "--cpu") == 0) && has_value) {
            options.cpu_backend = argv[++i];
        } else if (std::strcmp(argv[i], "--gpu") == 0) {
            gpu = true;
        } else if ((std::strcmp(argv[i], "--verify") == 0) && has_value) {
            verify_spec = argv[++i];
        } else if ((std::strcmp(argv[i], "--put") == 0) && has_value) {
            put_spec = argv[++i];
        } else if ((std::strcmp(argv[i], "--press-at") == 0) && has_value) {
            const std::string spec = argv[++i];
            const std::size_t colon = spec.find(':');

            if (colon == std::string::npos) {
                press_at = std::atoi(spec.c_str());
            } else {
                script.emplace_back(std::atoi(spec.substr(0, colon).c_str()),
                    std::atoi(spec.substr(colon + 1).c_str()));
            }
        } else if ((std::strcmp(argv[i], "--extract-to") == 0) && has_value) {
            extract_to = argv[++i];
            verbose = true;
        } else if ((std::strcmp(argv[i], "--bus-out") == 0) && has_value) {
            bus_out = argv[++i];
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

    // The machine gets somewhere to put its sound, exactly as the core gives
    // it one: with the media server alive, whether there is an audio driver
    // changes what the machine does.
    machine.start_audio();

    if (gpu) {
        char err[256] = { 0 };

        if (!chimera_egl_make_context(err, sizeof(err))) {
            std::fprintf(stderr, "no OpenGL context: %s\n", err);
            return 1;
        }

        machine.start_graphics(chimera_egl_proc);
        std::printf("gpu: %s\n", machine.has_graphics() ? "on" : "refused");
    } else {
        // The same machine the core is: a driver that accepts everything and
        // draws nowhere. Without one, an application that asks the window
        // server to compose takes the machine down with it.
        machine.start_null_graphics();
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

    // What the machine already had, so it can tell the project's own
    // application from the phone's afterwards.
    if (!install_path.empty() || !card_path.empty() || !blz_path.empty()) {
        machine.remember_apps();
    }

    if (!install_path.empty()) {
        // A package into the machine, the way the emulator's own installer
        // does it. Drive C is where a Symbian phone puts applications.
        const int result = machine.sys()->install_package(
            eka2l1::common::utf8_to_ucs2(install_path), drive_c);

        std::printf("install: %d\n", result);

        // The application list only learns about what an installer wrote if it
        // is told the drive changed under it.
        machine.sys()->get_io_system()->announce_drive(drive_c, eka2l1::drive_action_mount);
    }

    if (!blz_path.empty()) {
        // The same path the core takes: the installer application, driven, and
        // whatever it unpacks.
        std::printf("blz: %s\n", machine.install_blz(blz_path, blz_installer) ? "unpacked" : "refused");
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

    if (!put_spec.empty()) {
        // <machine path>=<host file>, written in through the machine's own
        // filesystem.
        const std::size_t eq = put_spec.find('=');

        if (eq != std::string::npos) {
            std::printf("put %s: %s\n", put_spec.substr(0, eq).c_str(),
                machine.put_file(put_spec.substr(0, eq), put_spec.substr(eq + 1)) ? "ok" : "refused");
        }
    }

    if (!verify_spec.empty()) {
        // Reads a file back OUT of the machine, the way a guest would - odd
        // sized reads, seeks between them - and compares it with the host file
        // it came from. A filesystem that serves the wrong bytes for an
        // unusual access pattern looks exactly like a corrupt download.
        const std::size_t eq = verify_spec.find('=');

        if (eq != std::string::npos) {
            const std::string machine_path = verify_spec.substr(0, eq);
            const std::string host_path = verify_spec.substr(eq + 1);

            std::vector<std::uint8_t> want;

            if (std::FILE *f = std::fopen(host_path.c_str(), "rb")) {
                std::uint8_t buf[65536];
                std::size_t got = 0;

                while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
                    want.insert(want.end(), buf, buf + got);
                }

                std::fclose(f);
            }

            eka2l1::symfile handle = machine.sys()->get_io_system()->open_file(
                eka2l1::common::utf8_to_ucs2(machine_path), READ_MODE | BIN_MODE);

            if (!handle) {
                std::printf("verify %s: cannot open\n", machine_path.c_str());
            } else {
                std::vector<std::uint8_t> got_bytes(want.size(), 0);
                std::size_t at = 0;
                std::size_t mismatch = SIZE_MAX;

                while (at < want.size()) {
                    const std::size_t chunk = std::min<std::size_t>(4093, want.size() - at);

                    handle->seek(static_cast<std::int64_t>(at), eka2l1::file_seek_mode::beg);

                    const std::size_t read = handle->read_file(got_bytes.data() + at,
                        static_cast<std::uint32_t>(chunk), 1);

                    if (read != chunk) {
                        std::printf("verify %s: short read at %zu (%zu of %zu)\n",
                            machine_path.c_str(), at, read, chunk);
                        break;
                    }

                    at += chunk;
                }

                for (std::size_t i = 0; i < want.size(); i++) {
                    if (got_bytes[i] != want[i]) {
                        mismatch = i;
                        break;
                    }
                }

                std::printf("verify %s: size %llu (host %zu) first difference %s\n", machine_path.c_str(),
                    static_cast<unsigned long long>(handle->size()), want.size(),
                    (mismatch == SIZE_MAX) ? "none" : std::to_string(mismatch).c_str());

                handle->close();
            }
        }
    }

    // The project's own application starts by itself, exactly as it does in
    // the core: a machine that was given a game runs the game.
    if (!card_path.empty() || !install_path.empty() || !blz_path.empty()) {
        const std::uint32_t launched = machine.launch_installed_app();

        if (launched != 0) {
            std::printf("launched: 0x%08x\n", launched);
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
                std::printf("app 0x%08x drive %c: %s\n", reg.mandatory_info.uid,
                    static_cast<char>('A' + static_cast<int>(reg.land_drive)),
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

                started = machine.launch_app(uid);
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
        // Held near the end of the run, not in the middle: what a key did is
        // read off the screen at the last frame, and a menu that has been left
        // alone for twenty emulated seconds has usually gone back to whatever
        // it does when nobody is there. Two hundred frames is long enough to
        // see the answer and short enough that it is still on screen.
        for (const auto &step : script) {
            if (i == step.first) {
                machine.set_button(step.second, true);
            } else if (i == step.first + 20) {
                machine.set_button(step.second, false);
            }
        }

        if (press_button >= 0) {
            const int at = (press_at >= 0) ? press_at : (frames - 200);

            if (i == at) {
                machine.set_button(press_button, true);
            } else if (i == ((press_at >= 0) ? press_at + 20 : frames - 180)) {
                machine.set_button(press_button, false);
            }
        }

        // A scan code straight into the window server, past the keypad map:
        // the only way to ask a machine which keys its application answers to,
        // rather than which keys the keypad was built to send.
        if ((press_raw >= 0) && ((i == frames - 200) || (i == frames - 180))) {
            eka2l1::window_server *ws = reinterpret_cast<eka2l1::window_server *>(
                machine.sys()->get_kernel_system()->get_by_name<eka2l1::service::server>(
                    eka2l1::get_winserv_name_by_epocver(machine.sys()->get_symbian_version_use())));

            if (ws) {
                eka2l1::drivers::input_event raw;

                raw.type_ = eka2l1::drivers::input_event_type::key_raw;
                raw.key_.state_ = (i == frames - 200) ? eka2l1::drivers::key_state::pressed
                                                        : eka2l1::drivers::key_state::released;
                raw.key_.code_ = static_cast<std::uint32_t>(press_raw);

                ws->queue_input_from_driver(raw);
            }
        }

        loops += machine.run_for_us(slice_us);

        // Pull a frame of sound, the way a core does. Pulling is not passive:
        // it runs the machine's own audio callbacks, and a reference that does
        // not pull is not running the same machine.
        {
            std::vector<std::int16_t> discarded(2 * (44100 / 60));
            machine.render_audio(discarded.data(), discarded.size() / 2);
        }

        // Read every frame, the way a core does: a machine drawing through
        // direct screen access waits for the screen to be put together, and a
        // composite that only happens when somebody reads is a composite the
        // machine never sees.
        {
            std::vector<std::uint32_t> ignored;
            int ignored_width = 0;
            int ignored_height = 0;

            machine.read_screen(ignored, ignored_width, ignored_height);
        }

        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }

    {
        // What the machine drew. The digest is over the pixels it left on the
        // panel, and the lit count is the shortest way to say whether anything
        // reached the screen at all.
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

    {
        // What the machine's own address space holds, byte by byte through the
        // page tables of the application this machine started - the machine's own address space, the way a watch tool reads it.

        // Two numbers, and only one of them is worth comparing between
        // flavors. The emulator keeps a little of its own bookkeeping inside
        // guest chunks - host pointers, eight bytes wide, at the base of the
        // chunks it allocates through - so the raw space differs between a
        // native process and a sandbox by a hundred-odd bytes that belong to
        // neither machine. The BODY of the game's heap has none of that in it,
        // and holds nine tenths of everything the machine has written.
        std::uint64_t digest = 1469598103934665603ull;
        std::size_t mapped = 0;
        std::size_t body = 0;

        for (std::uint32_t addr = 0; addr < BUS_SIZE; addr++) {
            const std::uint8_t byte = machine.peek_user(addr);

            if (byte != 0) {
                mapped++;
            }

            if ((addr >= BUS_BODY_FIRST) && (addr < BUS_BODY_LAST)) {
                digest ^= byte;
                digest *= 1099511628211ull;

                if (byte != 0) {
                    body++;
                }
            }
        }

        std::printf("bus: 64 MiB nonzero %zu\n", mapped);
        std::printf("bus body: digest %016llx nonzero %zu\n",
            static_cast<unsigned long long>(digest), body);

        if (!bus_out.empty()) {
            std::FILE *f = std::fopen(bus_out.c_str(), "wb");

            if (f) {
                for (std::uint32_t addr = 0; addr < BUS_SIZE; addr++) {
                    const std::uint8_t byte = machine.peek_user(addr);
                    std::fwrite(&byte, 1, 1, f);
                }

                std::fclose(f);
            }
        }
    }

    if (verbose && drives) {
        // Every file the machine holds. An installer's work is invisible
        // otherwise: there is no host directory to look in.
        drives->each_file([&](const std::string &path, const std::vector<std::uint8_t> &bytes) {
            std::printf("file: %-52s %8zu\n", path.c_str(), bytes.size());

            // And out to the host, if the caller wants to look inside one.
            if (!extract_to.empty()) {
                std::string flat = path;

                for (char &c : flat) {
                    if ((c == '\\') || (c == ':')) {
                        c = '_';
                    }
                }

                if (std::FILE *out = std::fopen((extract_to + "/" + flat).c_str(), "wb")) {
                    std::fwrite(bytes.data(), 1, bytes.size(), out);
                    std::fclose(out);
                }
            }
        });
    }

    if (verbose) {
        // Every window the compositor has, and whether it can be seen: a
        // window with an empty visible region draws nothing and gives a game
        // asking for direct screen access no area to draw into.
        eka2l1::kernel_system *kern_for_win = machine.sys()->get_kernel_system();
        eka2l1::window_server *winserv = kern_for_win ? reinterpret_cast<eka2l1::window_server *>(
            kern_for_win->get_by_name<eka2l1::service::server>(
                eka2l1::get_winserv_name_by_epocver(machine.sys()->get_symbian_version_use()))) : nullptr;

        if (winserv && winserv->get_screen(0) && winserv->get_screen(0)->root) {
            struct lister : public eka2l1::epoc::window_tree_walker {
                bool do_it(eka2l1::epoc::window *win) override {
                    if (win->type != eka2l1::epoc::window_kind::client) {
                        return false;
                    }

                    eka2l1::epoc::canvas_base *canvas =
                        reinterpret_cast<eka2l1::epoc::canvas_base *>(win);

                    std::printf("window: %4d %-8s %s%s%s rect %dx%d+%d+%d regions %zu\n", win->id,
                        canvas->is_visible() ? "visible" : "hidden",
                        (canvas->flags & eka2l1::epoc::window::flags_active) ? "active " : "inert  ",
                        (canvas->flags & eka2l1::epoc::window::flags_visible) ? "shown " : "unshown",
                        (canvas->flags & eka2l1::epoc::window::flags_dsa) ? " dsa" : "",
                        canvas->abs_rect.size.x, canvas->abs_rect.size.y,
                        canvas->abs_rect.top.x, canvas->abs_rect.top.y,
                        canvas->visible_region.rects_.size());

                    return false;
                }
            } walker;

            winserv->get_screen(0)->root->walk_tree_back_to_front(&walker);

            // Every chunk the game's process owns: the only shape guest RAM
            // has, and what a memory domain can be made of.
            eka2l1::kernel_system *kchunks = machine.sys()->get_kernel_system();

            kchunks->for_each_chunk([&](eka2l1::kernel::chunk *c) {
                if (!c || !c->valid() || !c->host_base()) {
                    return;
                }

                eka2l1::kernel::process *own = c->get_own_process();

                std::printf("chunk: %-28s owner %-12s base 0x%08x max %8zu committed %8zu%s\n",
                    c->name().c_str(),
                    own ? own->name().c_str() : "-",
                    c->base(own).ptr_address(),
                    c->max_size(), c->committed(),
                    c->is_chunk_heap() ? " heap" : "");
            });

            // And the window groups: which applications the machine has on
            // screen, and which one the keys go to.
            eka2l1::epoc::screen *scr0 = winserv->get_screen(0);
            const eka2l1::epoc::window_group *focused = scr0->focus;

            for (eka2l1::epoc::window *w = scr0->root->child; w != nullptr; w = w->sibling) {
                if (w->type != eka2l1::epoc::window_kind::group) {
                    continue;
                }

                eka2l1::epoc::window_group *grp = reinterpret_cast<eka2l1::epoc::window_group *>(w);

                std::printf("group: %4d %-8s %-9s %s\n", grp->id,
                    (grp == focused) ? "FOCUSED" : "",
                    grp->can_receive_focus() ? "focusable" : "no-focus",
                    eka2l1::common::ucs2_to_utf8(grp->name).c_str());
            }
        }

        // Every thread the machine has and what it is doing. A machine that
        // has gone quiet is a machine where every thread is waiting, and this
        // says which ones and for what kind of thing.
        static const char *const STATES[] = {
            "create", "run", "wait", "ready", "stop", "wait-fast-sema", "wait-mutex",
            "wait-condvar", "wait-mutex-suspend", "wait-fast-sema-suspend",
            "wait-condvar-suspend", "hold-mutex-pending", "wait-dfc", "wait-hle"
        };

        for (auto &object : machine.sys()->get_kernel_system()->get_thread_list()) {
            eka2l1::kernel::thread *thread = reinterpret_cast<eka2l1::kernel::thread *>(object.get());

            if (!thread) {
                continue;
            }

            const int state = static_cast<int>(thread->current_state());

            // What it waits on, and how many requests it has outstanding: a
            // thread in WaitForAnyRequest with a negative count is a thread
            // owed a completion that never came.
            std::printf("thread: %-28s %-16s requests %d on %s\n", thread->name().c_str(),
                ((state >= 0) && (state < 14)) ? STATES[state] : "?",
                thread->request_count(),
                thread->wait_obj ? thread->wait_obj->name().c_str() : "-");
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
