// EKA2L1 as a Chimera waterbox core: the entry points miniBox calls.
//
// At this milestone the machine inside the sandbox is the empty one - no
// device dump has been handed across yet - so what it does is what an empty
// machine can do: hold a kernel timer, and let virtual time carry it. That is
// deliberately the same workload run-native drives, so the two builds can be
// compared line for line, which is the only way to know the emulator behaves
// identically inside the box and outside it.
#include "machine.h"

#include <kernel/timing.h>
#include <system/epoc.h>

#include <cstdint>
#include <cstdio>
#include <memory>

#include <emulibc.h>
#include <waterboxcore.h>

namespace {
    // The N-Gage's screen. Nothing draws to it yet; it is here so a host that
    // asks for a picture gets a well-formed one rather than nothing.
    constexpr int SCREEN_WIDTH = 176;
    constexpr int SCREEN_HEIGHT = 208;

    // The workload: a kernel timer every millisecond, matching
    // `run-native --timer-us 1000`.
    constexpr int TIMER_PERIOD_US = 1000;

    char g_loadError[512] = { 0 };
    bool g_inited = false;

    std::unique_ptr<chimera::machine> g_machine;

    std::uint64_t g_timerFired = 0;
    std::uint64_t g_timerLastUs = 0;
    std::uint64_t g_timerLatenessUs = 0;
    int g_timerEvent = 0;

    std::uint32_t g_video[SCREEN_WIDTH * SCREEN_HEIGHT];
    std::int16_t g_audio[2] = { 0, 0 };
}

extern "C" {

ECL_EXPORT const char *GetLoadError(void) {
    return g_loadError;
}

ECL_EXPORT int Init(void) {
    g_loadError[0] = '\0';

    chimera::machine_options options;
    options.storage = "data";

    g_machine = std::make_unique<chimera::machine>(options);
    g_machine->startup();

    eka2l1::ntimer *timing = g_machine->sys()->get_ntimer();

    g_timerEvent = timing->register_event("chimera:timer-check",
        [timing](const std::uint64_t userdata, const int late) {
            (void)userdata;

            g_timerFired++;
            g_timerLastUs = timing->microseconds();
            g_timerLatenessUs += static_cast<std::uint64_t>(late);

            timing->schedule_event(TIMER_PERIOD_US, g_timerEvent, 0);
        });

    timing->schedule_event(TIMER_PERIOD_US, g_timerEvent, 0);

    g_inited = true;
    return 1;
}

ECL_EXPORT void FrameAdvance(std::uint64_t) {
    if (!g_inited) {
        return;
    }

    g_machine->run_for_us(1000000ull / 60ull);
}

ECL_EXPORT std::uint32_t *GetVideoBgra(void) {
    return g_video;
}

ECL_EXPORT int GetVideoWidth(void) {
    return SCREEN_WIDTH;
}

ECL_EXPORT int GetVideoHeight(void) {
    return SCREEN_HEIGHT;
}

ECL_EXPORT std::int16_t *GetAudio(void) {
    return g_audio;
}

ECL_EXPORT int GetAudioSampleCount(void) {
    return 0;
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

}
