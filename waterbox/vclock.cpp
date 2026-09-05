#include "vclock.h"

#include <common/time.h>

namespace chimera {
    virtual_clock::virtual_clock(const std::uint64_t epoch_us, const std::uint64_t instructions_per_second)
        : epoch_us_(epoch_us)
        , now_us_(epoch_us)
        , instructions_per_second_(instructions_per_second ? instructions_per_second : 1)
        , remainder_(0)
        , instructions_(0) {
    }

    void virtual_clock::install() {
        eka2l1::common::set_utc_time_source([this]() {
            return now_us_;
        });
    }

    void virtual_clock::uninstall() {
        eka2l1::common::set_utc_time_source(nullptr);
    }

    void virtual_clock::advance_us(const std::uint64_t us) {
        now_us_ += us;
    }

    void virtual_clock::spend_instructions(const std::uint32_t instructions) {
        instructions_ += instructions;

        // instructions * 1e6 is at most 4.3e15, which a 64-bit count holds with
        // room to spare, so the division needs no wider arithmetic.
        const std::uint64_t owed = remainder_ + (static_cast<std::uint64_t>(instructions) * 1000000ull);

        now_us_ += owed / instructions_per_second_;
        remainder_ = owed % instructions_per_second_;
    }
}
