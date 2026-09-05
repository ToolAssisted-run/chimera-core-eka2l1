#pragma once

// The machine's sound, as a core takes it.
//
// EKA2L1 hands sound out the way a host audio library asks for it: a stream
// with a callback the device pulls when it wants more. A core has no device
// and no clock but its own, so it pulls exactly what one frame is worth,
// exactly when the frame ends, and every stream is summed into that.
#include <drivers/audio/audio.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace chimera {
    class audio_stream;

    class audio_sink : public eka2l1::drivers::audio_driver {
    public:
        explicit audio_sink(const std::uint32_t sample_rate);
        ~audio_sink() override;

        std::unique_ptr<eka2l1::drivers::audio_output_stream> new_output_stream(const std::uint32_t sample_rate,
            const std::uint8_t channels, eka2l1::drivers::data_callback callback) override;

        std::unique_ptr<eka2l1::drivers::audio_input_stream> new_input_stream(const std::uint32_t sample_rate,
            const std::uint8_t channels, eka2l1::drivers::data_callback callback) override;

        std::uint32_t native_sample_rate() override {
            return sample_rate_;
        }

        // Sums `frames` stereo frames from every playing stream into `out`.
        // Streams that want a different rate are stepped at their own and
        // resampled by holding, which is what a machine with no clock of its
        // own can honestly do.
        void render(std::int16_t *out, const std::size_t frames);

        void add(audio_stream *stream);
        void remove(audio_stream *stream);

    private:
        std::uint32_t sample_rate_;
        std::vector<audio_stream *> streams_;
        std::vector<std::int16_t> scratch_;
    };
}
