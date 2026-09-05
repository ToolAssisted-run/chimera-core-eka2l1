#include "audio.h"

#include <algorithm>

namespace chimera {
    class audio_stream : public eka2l1::drivers::audio_output_stream {
    public:
        audio_stream(audio_sink *sink, const std::uint32_t sample_rate, const std::uint8_t channels,
            eka2l1::drivers::data_callback callback)
            : eka2l1::drivers::audio_output_stream(sink, sample_rate, channels)
            , sink_(sink)
            , callback_(std::move(callback))
            , playing_(false)
            , volume_(1.0f)
            , position_(0) {
            sink_->add(this);
        }

        ~audio_stream() override {
            sink_->remove(this);
        }

        bool start() override {
            playing_ = true;
            return true;
        }

        bool stop() override {
            playing_ = false;
            return true;
        }

        void pause() override {
            playing_ = false;
        }

        bool is_playing() override {
            return playing_;
        }

        bool is_pausing() override {
            return !playing_;
        }

        bool set_volume(const float volume) override {
            volume_ = volume;
            return true;
        }

        float get_volume() const override {
            return volume_;
        }

        bool current_frame_position(std::uint64_t *pos) override {
            if (pos) {
                *pos = position_;
            }

            return true;
        }

        // Pulls `frames` of this stream's own frames and adds them to `out` as
        // stereo at the sink's rate.
        void mix_into(std::int16_t *out, const std::size_t frames, const std::uint32_t out_rate,
            std::vector<std::int16_t> &scratch) {
            if (!playing_ || !callback_) {
                return;
            }

            // How many of this stream's frames a frame of ours is worth.
            const std::size_t want = out_rate ? static_cast<std::size_t>(
                (static_cast<std::uint64_t>(frames) * sample_rate + out_rate - 1) / out_rate) : frames;

            scratch.assign(want * channels, 0);

            const std::size_t got = callback_(scratch.data(), want);

            if (got == 0) {
                return;
            }

            position_ += got;

            for (std::size_t i = 0; i < frames; i++) {
                // Nearest source frame, held. No filter: a core may not invent
                // samples the machine did not make.
                const std::size_t source = (want == frames) ? i : (i * want / frames);

                if (source >= got) {
                    break;
                }

                const std::int32_t left = scratch[source * channels];
                const std::int32_t right = (channels > 1) ? scratch[source * channels + 1] : left;

                const std::int32_t mixed_left = out[i * 2] + static_cast<std::int32_t>(left * volume_);
                const std::int32_t mixed_right = out[i * 2 + 1] + static_cast<std::int32_t>(right * volume_);

                out[i * 2] = static_cast<std::int16_t>(std::clamp(mixed_left, -32768, 32767));
                out[i * 2 + 1] = static_cast<std::int16_t>(std::clamp(mixed_right, -32768, 32767));
            }
        }

    private:
        audio_sink *sink_;
        eka2l1::drivers::data_callback callback_;
        bool playing_;
        float volume_;
        std::uint64_t position_;
    };

    audio_sink::audio_sink(const std::uint32_t sample_rate)
        : eka2l1::drivers::audio_driver(100)
        , sample_rate_(sample_rate) {
    }

    audio_sink::~audio_sink() = default;

    std::unique_ptr<eka2l1::drivers::audio_output_stream> audio_sink::new_output_stream(
        const std::uint32_t sample_rate, const std::uint8_t channels, eka2l1::drivers::data_callback callback) {
        return std::make_unique<audio_stream>(this, sample_rate, channels, std::move(callback));
    }

    std::unique_ptr<eka2l1::drivers::audio_input_stream> audio_sink::new_input_stream(
        const std::uint32_t sample_rate, const std::uint8_t channels, eka2l1::drivers::data_callback callback) {
        // There is no microphone in a sandbox.
        (void)sample_rate;
        (void)channels;
        (void)callback;

        return nullptr;
    }

    void audio_sink::add(audio_stream *stream) {
        streams_.push_back(stream);
    }

    void audio_sink::remove(audio_stream *stream) {
        streams_.erase(std::remove(streams_.begin(), streams_.end(), stream), streams_.end());
    }

    void audio_sink::render(std::int16_t *out, const std::size_t frames) {
        std::fill(out, out + frames * 2, static_cast<std::int16_t>(0));

        // A copy: a stream may end while it is being mixed, and ending removes
        // it from the list.
        const std::vector<audio_stream *> playing = streams_;

        for (audio_stream *stream : playing) {
            stream->mix_into(out, frames, sample_rate_, scratch_);
        }
    }
}
