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
#include <kernel/timing.h>
#include <system/devices.h>
#include <system/epoc.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <thread>

int main(int argc, char **argv) {
    chimera::machine_options options;
    options.storage = "data";

    int frames = 0;
    int fps = 60;
    int sleep_ms = 0;
    int timer_period_us = 0;

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
        std::printf("device 0: %s\n", machine.set_device(0) ? "set" : "refused");
    }

    // The memory model follows the device's Symbian version, so the MMU only
    // exists once a device has been set.
    std::printf("memory: %s\n", machine.sys()->get_memory_system() ? "up" : "absent, no device");

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

        for (const auto &entry : tasks) {
            (void)entry;
            threads++;
        }

        std::printf("host threads: %zu\n", threads);
    }

    std::printf("teardown: ok\n");

    return 0;
}
