#pragma once

#include <cstdint>

namespace chimera {
    // The machine's clock. It is virtual: it moves when the machine moves, and
    // at no other time. Nothing here reads the host.
    //
    // EKA2L1 has no cycle model - loop() runs the current thread for its
    // quantum and returns - so time is bought with instructions at a declared
    // rate, the way a machine with a known clock speed spends them. Idle time,
    // where no instruction is executed at all, is bought outright by the step
    // loop when it jumps to the next timer deadline.
    class virtual_clock {
    public:
        // `epoch_us` is what the machine believes the date is when it starts,
        // in microseconds since 1/1/1970; `instructions_per_second` is how fast
        // it believes its own processor to be.
        virtual_clock(const std::uint64_t epoch_us, const std::uint64_t instructions_per_second);

        // Becomes the emulator's source of "now". Only one clock can hold it,
        // and it must outlive the machine that reads it.
        void install();
        static void uninstall();

        // Time the machine did not spend executing: an idle jump to the next
        // timer deadline, or the tail of a frame with nothing left to run.
        void advance_us(const std::uint64_t us);

        // Time bought with work. The fraction of a microsecond left over is
        // kept, so a million one-instruction steps cost exactly what one
        // million-instruction step costs.
        void spend_instructions(const std::uint32_t instructions);

        std::uint64_t now_us() const {
            return now_us_;
        }

        std::uint64_t elapsed_us() const {
            return now_us_ - epoch_us_;
        }

        std::uint64_t instructions() const {
            return instructions_;
        }

    private:
        std::uint64_t epoch_us_;
        std::uint64_t now_us_;
        std::uint64_t instructions_per_second_;
        std::uint64_t remainder_;
        std::uint64_t instructions_;
    };
}
