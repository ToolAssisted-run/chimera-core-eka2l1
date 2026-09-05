// The native reference harness: it constructs the emulator the way the Qt
// frontend's stage_one does, minus Qt and minus every host device, and reports
// what it sees. No graphics driver, no audio driver, no window: the point of
// this program is that the machine can be built, questioned and torn down by an
// embedder, and that two runs say exactly the same thing.
//
//   run-native --data <storage root> [--list-devices]
//
// Everything it prints is derived from the emulator, never from the host clock,
// so its output is the first determinism check this port has.
#include <config/app_settings.h>
#include <config/config.h>
#include <common/log.h>
#include <system/devices.h>
#include <system/epoc.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

int main(int argc, char **argv) {
    std::string storage = "data";

    for (int i = 1; i < argc; i++) {
        if ((std::strcmp(argv[i], "--data") == 0) && (i + 1 < argc)) {
            storage = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    eka2l1::log::setup_log(nullptr);

    eka2l1::config::state conf;
    conf.storage = storage;

    // The machine is the reference, so nothing it does may depend on the host.
    // The interpreter is the only backend with an agreement leg behind it, the
    // scheduler must return rather than sleep when no thread is runnable, and
    // the bitmap compressor must not run on a thread of its own.
    conf.cpu_backend = "dyncom";
    conf.cpu_load_save = false;
    conf.fbs_enable_compression_queue = false;

    eka2l1::config::app_settings settings(&conf);

    eka2l1::system_create_components comp;
    comp.graphics_ = nullptr;
    comp.audio_ = nullptr;
    comp.conf_ = &conf;
    comp.settings_ = &settings;
    comp.cache_root_ = storage;

    auto sys = std::make_unique<eka2l1::system>(comp);

    eka2l1::device_manager *devices = sys->get_device_manager();
    const std::size_t total = devices->total();

    std::printf("storage: %s\n", storage.c_str());
    std::printf("cpu: %s\n", conf.cpu_backend.c_str());
    std::printf("devices: %zu\n", total);

    for (std::size_t i = 0; i < total; i++) {
        const eka2l1::device &dvc = devices->get_devices()[i];
        std::printf("device %zu: %s %s (%s) epocver=%d\n", i, dvc.manufacturer.c_str(),
            dvc.model.c_str(), dvc.firmware_code.c_str(), static_cast<int>(dvc.ver));
    }

    // startup() needs no device: it builds the timer, the physical filesystem,
    // the exclusive monitor, the CPU core and the kernel. Whether there is a
    // device to boot afterwards is a separate question, and the answer to it is
    // the user's to supply.
    sys->startup();

    static const char *const backend_names[] = { "unicorn", "dynarmic", "12l1r", "dyncom" };
    const int backend = static_cast<int>(sys->get_cpu_executor_type());

    std::printf("startup: ok\n");
    std::printf("cpu resolved: %s\n", (backend >= 0 && backend < 4) ? backend_names[backend] : "unknown");
    std::printf("kernel: %s\n", sys->get_kernel_system() ? "up" : "absent");
    std::printf("timer: %s\n", sys->get_ntimer() ? "up" : "absent");

    if (total > 0) {
        std::printf("device 0: %s\n", sys->set_device(0) ? "set" : "refused");
    }

    // The memory model follows the device's Symbian version, so the MMU only
    // exists once a device has been set.
    std::printf("memory: %s\n", sys->get_memory_system() ? "up" : "absent, no device");

    sys.reset();
    std::printf("teardown: ok\n");

    return 0;
}
