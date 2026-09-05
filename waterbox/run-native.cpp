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

#include <common/log.h>
#include <common/cvt.h>
#include <kernel/kernel.h>
#include <kernel/timing.h>
#include <services/applist/applist.h>
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

int main(int argc, char **argv) {
    chimera::machine_options options;
    options.storage = "data";

    int frames = 0;
    int fps = 60;
    int sleep_ms = 0;
    int timer_period_us = 0;
    bool list_apps = false;
    std::string run_path;

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

    chimera::machine machine(options);

    const std::size_t devices = machine.device_count();

    std::printf("storage: %s\n", options.storage.c_str());
    std::printf("clock: %s\n", options.host_clock ? "the host's" : "virtual");
    std::printf("devices: %zu\n", devices);

    for (std::size_t i = 0; i < devices; i++) {
        const eka2l1::device &dvc = machine.sys()->get_device_manager()->get_devices()[i];
        std::printf("device %zu: %s %s (%s) epocver=%d\n", i, dvc.manufacturer.c_str(),
            dvc.model.c_str(), dvc.firmware_code.c_str(), static_cast<int>(dvc.ver));
    }

    // startup() needs no device: it builds the timer, the physical filesystem,
    // the exclusive monitor, the CPU core and the kernel. Whether there is a
    // device to boot afterwards is a separate question, and the answer to it is
    // the user's to supply.
    machine.startup();

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
            std::printf("boot: ok\n");
        }
    }

    // The memory model follows the device's Symbian version, so the MMU only
    // exists once a device has been set.
    std::printf("memory: %s\n", machine.sys()->get_memory_system() ? "up" : "absent, no device");

    if (list_apps || !run_path.empty()) {
        eka2l1::kernel_system *kern = machine.sys()->get_kernel_system();
        eka2l1::applist_server *applist = reinterpret_cast<eka2l1::applist_server *>(
            kern->get_by_name<eka2l1::service::server>(
                eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));

        if (!applist) {
            std::printf("apps: no application list server\n");
        } else if (list_apps) {
            std::vector<eka2l1::apa_app_registry> &regs = applist->get_registerations();
            std::printf("apps: %zu\n", regs.size());

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
        loops += machine.run_for_us(slice_us);

        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
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
