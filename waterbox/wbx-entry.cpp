// EKA2L1 as a Chimera waterbox core: the entry points miniBox calls.
//
// The device arrives as ONE mounted file: the ROM. It says which device it is,
// it carries drive Z, and the machine maps it. The writable drives are the
// machine's own memory (memfs.cpp). Without it the machine is still built and
// still keeps time, which is what the equivalence gate compares when no ROM is
// present.
#include <cstring>
#include "input.h"
#include "machine.h"
#include "memfs.h"

#include <common/cvt.h>
#include <common/path.h>
#include <kernel/kernel.h>
#include <kernel/timing.h>
#include <system/consts.h>
#include <utils/apacmd.h>
#include <services/applist/applist.h>
#include <system/devices.h>
#include <system/epoc.h>
#include <vfs/vfs.h>

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <emulibc.h>
#include <waterbox_slots.h>
#include <waterboxcore.h>

namespace {
    // The N-Gage's screen. Nothing draws to it yet; it is here so a host that
    // asks for a picture gets a well-formed one rather than nothing.
    constexpr int SCREEN_WIDTH = 176;
    constexpr int SCREEN_HEIGHT = 208;

    char g_loadError[512] = { 0 };
    bool g_inited = false;

    std::unique_ptr<chimera::machine> g_machine;

    std::uint64_t g_timerFired = 0;
    std::uint64_t g_timerLastUs = 0;
    std::uint64_t g_timerLatenessUs = 0;
    int g_timerEvent = 0;
    int g_timerPeriodUs = 0;

    // The picture and the sound of one frame. The screen is the N-Gage's own
    // 176x208; the buffer is the largest a Symbian screen this core will run
    // can be, so a device with a bigger panel still fits.
    constexpr int MAX_WIDTH = 640;
    constexpr int MAX_HEIGHT = 640;

    constexpr std::uint32_t SAMPLE_RATE = 44100;
    constexpr int MAX_SAMPLES = 2048;

    std::vector<std::uint32_t> g_screen;
    std::uint32_t g_video[MAX_WIDTH * MAX_HEIGHT];
    int g_videoWidth = SCREEN_WIDTH;
    int g_videoHeight = SCREEN_HEIGHT;

    std::int16_t g_audio[MAX_SAMPLES * 2];
    int g_audioSamples = 0;

    std::shared_ptr<chimera::memory_file_system> g_drives;
    bool g_device = false;
    std::uint32_t g_launched = 0;

    // The machine, as firmware: a dump of a phone's own ROM, which says which
    // device this is and carries drive Z whole. One ROM serves every game, so
    // it is not the project's file.
    constexpr const char *ROM_NAME = "sym.rom";

    // The project's file: the game.
    constexpr const char *GAME_SLOT = "game";

    // And, for a game that arrives as a .blz, the Symbian application that can
    // unpack one. Firmware too, and asked for only when the project's file is
    // a .blz.
    constexpr const char *BLZ_INSTALLER = "blzinstapp.sis";

    int g_installed = -1;

    bool ends_with(const char *name, const char *suffix) {
        const std::size_t n = std::strlen(name);
        const std::size_t m = std::strlen(suffix);

        if (n < m) {
            return false;
        }

        for (std::size_t i = 0; i < m; i++) {
            const char a = name[n - m + i];
            const char b = suffix[i];

            if (((a >= 'A') && (a <= 'Z') ? (a + 32) : a) != b) {
                return false;
            }
        }

        return true;
    }
}

extern "C" {

// The guest's own OpenGL (waterbox/gl-osmesa.c).
int chimera_osmesa_start(int width, int height);
void *chimera_osmesa_proc(const char *name);

ECL_EXPORT const char *GetLoadError(void) {
    return g_loadError;
}

ECL_EXPORT int Init(void) {
    g_loadError[0] = '\0';

    chimera::machine_options options;
    options.storage = "data";

    options.in_memory_drives = true;
    options.sample_rate = SAMPLE_RATE;

    g_machine = std::make_unique<chimera::machine>(options);
    g_machine->startup();

    // The machine's OpenGL. There is no GPU here and no host driver to borrow
    // one from, so the core carries its own: Mesa's softpipe, compiled in. It
    // is what an application that draws through the window server needs, and
    // there are plenty - a game that paints the panel itself does not, which
    // is why this was not always here.
    //
    // If it cannot be had, a driver that draws nowhere at least keeps the
    // machine alive: a window server with no driver at all dies on the first
    // canvas anybody asks it to compose.
    if (chimera_osmesa_start(MAX_WIDTH, MAX_HEIGHT)) {
        g_machine->start_graphics(chimera_osmesa_proc);
    }

    if (!g_machine->has_graphics()) {
        g_machine->start_null_graphics();
    }
    g_machine->start_audio();

    // The drives, before the device: setting the device loads the ROM and asks
    // every filesystem about the product code, and ours has to be one of them.
    g_drives = std::make_shared<chimera::memory_file_system>();

    eka2l1::file_system_inst as_instance = g_drives;
    g_machine->sys()->get_io_system()->add_filesystem(as_instance);

    if (g_machine->add_device_from_rom(ROM_NAME)) {
        // Writable drives for whatever the machine puts on them. They are
        // empty, they are the machine's, and they travel in its savestates.
        g_drives->mount_empty(drive_c, drive_media::physical, io_attrib_internal);
        g_drives->mount_empty(drive_d, drive_media::physical, io_attrib_internal);
        g_drives->mount_empty(drive_e, drive_media::physical, io_attrib_removeable);

        if (!g_machine->set_device(0)) {
            std::snprintf(g_loadError, sizeof g_loadError, "the device in the ROM was refused");
            return 0;
        }

        g_machine->boot();

        // The drives were mounted on a filesystem of their own, which nothing
        // else could have been told about. The application list only scans a
        // drive it has been told about.
        for (const drive_number drv : { drive_c, drive_d, drive_e }) {
            g_machine->sys()->get_io_system()->announce_drive(drv, eka2l1::drive_action_mount);
        }

        // A package, if the project brought one. It installs into the
        // machine's own drive C, which lives in the machine's memory and
        // therefore in its savestates.
        char slot_name[256] = { 0 };
        const char *game = nullptr;

        if (wbx_slot_name(GAME_SLOT, 0, slot_name, sizeof slot_name) != nullptr) {
            game = slot_name;
        } else if (std::FILE *plain = std::fopen(GAME_SLOT, "rb")) {
            std::fclose(plain);
            game = GAME_SLOT;
        }

        if (game != nullptr) {
            // What the machine already had, so it can tell the project's own
            // application from the phone's afterwards.
            g_machine->remember_apps();

            // A card image the machine cannot read for itself, unpacked by the
            // Symbian application that can; a game card - an archive holding a
            // System\Apps tree; or a Symbian package. In that order, because
            // only the first is told apart by its name.
            if (ends_with(game, ".blz")) {
                if (!g_machine->install_blz(game, BLZ_INSTALLER)) {
                    std::snprintf(g_loadError, sizeof g_loadError,
                        "the machine could not unpack this .blz - the BLZ installer firmware "
                        "did not run, or unpacked nothing");
                    return 0;
                }

                g_installed = 0;
            } else {
                const int card_files = g_machine->install_card(game);

                if (card_files > 0) {
                    g_installed = 0;

                    g_machine->sys()->get_io_system()->announce_drive(drive_e, eka2l1::drive_action_mount);
                } else {
                    g_installed = g_machine->install_package(game);

                    // An installer writes to drive C, and the application list
                    // only learns of it if the drive is announced again.
                    g_machine->sys()->get_io_system()->announce_drive(drive_c, eka2l1::drive_action_mount);
                }
            }

            // And it starts. A phone with nothing running shows its menu, and
            // a project that brought a game means the game: the frontend has
            // no launcher to offer and the machine should not need one.
            g_launched = g_machine->launch_installed_app();
        }

        g_device = true;
    }

    g_inited = true;
    return 1;
}

// A kernel timer of the host's asking, every `us` microseconds. Not something
// a machine has of its own: it is how the equivalence gate gives an empty
// machine - one with no device - something to do that both flavors can be
// compared on. Zero, the default, registers nothing.
ECL_EXPORT void SetTimerCheckUs(std::int32_t us) {
    if (!g_inited || (us <= 0) || (g_timerEvent != 0)) {
        return;
    }

    eka2l1::ntimer *timing = g_machine->sys()->get_ntimer();

    g_timerPeriodUs = us;
    g_timerEvent = timing->register_event("chimera:timer-check",
        [timing](const std::uint64_t userdata, const int late) {
            (void)userdata;

            g_timerFired++;
            g_timerLastUs = timing->microseconds();
            g_timerLatenessUs += static_cast<std::uint64_t>(late);

            timing->schedule_event(g_timerPeriodUs, g_timerEvent, 0);
        });

    timing->schedule_event(g_timerPeriodUs, g_timerEvent, 0);
}

ECL_EXPORT void FrameAdvance(std::uint64_t) {
    if (!g_inited) {
        return;
    }

    g_machine->run_for_us(1000000ull / 60ull);

    // The sound the machine made during it, and the picture it left behind.
    g_audioSamples = static_cast<int>(SAMPLE_RATE / 60);
    g_machine->render_audio(g_audio, static_cast<std::size_t>(g_audioSamples));

    int width = 0;
    int height = 0;

    if (g_machine->read_screen(g_screen, width, height) && (width > 0) && (height > 0)
        && (width <= MAX_WIDTH) && (height <= MAX_HEIGHT)) {
        g_videoWidth = width;
        g_videoHeight = height;

        std::copy(g_screen.begin(), g_screen.end(), g_video);
    }
}

// Levels, not events: the host says what is held this frame.
ECL_EXPORT void SetButton(std::int32_t index, std::int32_t state) {
    if (g_inited) {
        g_machine->set_button(index, state != 0);
    }
}

ECL_EXPORT std::uint32_t *GetVideoBgra(void) {
    return g_video;
}

ECL_EXPORT int GetVideoWidth(void) {
    return g_videoWidth;
}

ECL_EXPORT int GetVideoHeight(void) {
    return g_videoHeight;
}

ECL_EXPORT std::int16_t *GetAudio(void) {
    return g_audio;
}

ECL_EXPORT int GetAudioSampleCount(void) {
    return g_audioSamples;
}

// The machine keeps its state in the sandbox's own memory, which the host
// snapshots whole; nothing here is a domain of its own yet.
ECL_EXPORT int GetMemoryDomainCount(void) {
    return 0;
}

ECL_EXPORT const char *GetMemoryDomainName(std::int32_t) {
    return "";
}

ECL_EXPORT std::uint8_t *GetMemoryDomainPtr(std::int32_t) {
    return nullptr;
}

ECL_EXPORT std::int64_t GetMemoryDomainSize(std::int32_t) {
    return 0;
}

ECL_EXPORT int GetMemoryDomainWritable(std::int32_t) {
    return 0;
}

// ---- the machine's memory, as an address space -----------------------------
//
// Not a block: Symbian gives every chunk its own mapping and a game's chunks do
// not exist until it has run, so there is nothing whose address and size could
// be handed over once and stay true. A bus is resolved per access instead, in
// the address space of the application this machine started.
//
// Sixty-four megabytes from zero: EKA1 puts a process's own memory at
// 0x00400000 and gives it the space below the shared region, and every chunk
// this game makes lands in the first few. Addresses are the machine's own, so
// what a watch says is what a debugger would say.
namespace {
    constexpr std::int64_t USER_BUS_SIZE = 0x04000000;
}

ECL_EXPORT std::int32_t GetBusCount(void) {
    return g_device ? 1 : 0;
}

ECL_EXPORT const char *GetBusName(std::int32_t) {
    return "User RAM";
}

ECL_EXPORT std::int64_t GetBusSize(std::int32_t) {
    return USER_BUS_SIZE;
}

ECL_EXPORT std::int32_t GetBusWritable(std::int32_t) {
    return 1;
}

ECL_EXPORT std::int32_t PeekBus(std::int32_t bus, std::int32_t addr) {
    if (!g_inited || (bus != 0) || (addr < 0) || (addr >= USER_BUS_SIZE)) {
        return 0;
    }

    return g_machine->peek_user(static_cast<std::uint32_t>(addr));
}

ECL_EXPORT void PokeBus(std::int32_t bus, std::int32_t addr, std::int32_t value) {
    if (!g_inited || (bus != 0) || (addr < 0) || (addr >= USER_BUS_SIZE)) {
        return;
    }

    g_machine->poke_user(static_cast<std::uint32_t>(addr), static_cast<std::uint8_t>(value));
}

ECL_EXPORT int GetVsyncNumerator(void) {
    return 60;
}

ECL_EXPORT int GetVsyncDenominator(void) {
    return 1;
}

// What the machine did, in its own terms. The equivalence gate compares these
// against the native reference's, so every one of them must come from the
// machine and none from the host.
ECL_EXPORT std::uint64_t GetVirtualUs(void) {
    return g_inited ? g_machine->clock().elapsed_us() : 0;
}

ECL_EXPORT std::uint64_t GetInstructions(void) {
    return g_inited ? g_machine->clock().instructions() : 0;
}

ECL_EXPORT std::uint64_t GetTimerFired(void) {
    return g_timerFired;
}

ECL_EXPORT std::uint64_t GetTimerLastUs(void) {
    return g_timerLastUs;
}

ECL_EXPORT std::uint64_t GetTimerLatenessUs(void) {
    return g_timerLatenessUs;
}

// Starts an application the way the machine's own launcher would, through its
// registration rather than by opening a file. Answers 1 when the machine took
// it. Meant for the equivalence gate, which runs the same application here and
// in the native reference and compares what the processor did.
ECL_EXPORT int LaunchAppUid(std::uint32_t uid) {
    if (!g_device) {
        return 0;
    }

    // Init starts the project's own application. Asking for that one again is
    // answered rather than obeyed: a second copy of a running game would be a
    // second process, not a restart.
    if (uid == g_launched) {
        return 1;
    }

    eka2l1::kernel_system *kern = g_machine->sys()->get_kernel_system();
    eka2l1::applist_server *applist = reinterpret_cast<eka2l1::applist_server *>(
        kern->get_by_name<eka2l1::service::server>(
            eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));

    if (!applist) {
        return 0;
    }

    eka2l1::apa_app_registry *registry = applist->get_registration(uid);

    if (!registry) {
        return 0;
    }

    eka2l1::epoc::apa::command_line cmdline;
    cmdline.launch_cmd_ = eka2l1::epoc::apa::command_create;

    return applist->launch_app(*registry, cmdline, nullptr, nullptr) ? 1 : 0;
}

// Which application the machine started for itself, or zero.
ECL_EXPORT std::uint32_t GetLaunchedAppUid(void) {
    return g_launched;
}

ECL_EXPORT std::uint64_t GetAppCount(void) {
    if (!g_device) {
        return 0;
    }

    eka2l1::kernel_system *kern = g_machine->sys()->get_kernel_system();
    eka2l1::applist_server *applist = reinterpret_cast<eka2l1::applist_server *>(
        kern->get_by_name<eka2l1::service::server>(
            eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));

    return applist ? applist->get_registerations().size() : 0;
}

// The device, if one was handed across: how much of a filesystem the machine
// is holding, and how much of it is the machine's own rather than the pack's.
ECL_EXPORT int GetDeviceMounted(void) {
    return g_device ? 1 : 0;
}

ECL_EXPORT std::uint64_t GetDriveEntries(void) {
    return g_drives ? g_drives->entry_count() : 0;
}

ECL_EXPORT std::uint64_t GetDriveWrittenBytes(void) {
    return g_drives ? g_drives->written_bytes() : 0;
}

// What the package install said: -1 for "there was none", 0 for installed.
ECL_EXPORT int GetInstallResult(void) {
    return g_installed;
}

}
