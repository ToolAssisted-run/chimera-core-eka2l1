#include "input.h"

namespace chimera {
    namespace {
        const char *const NAMES[BUTTON_COUNT] = {
            "Up", "Down", "Left", "Right", "Select",
            "Left Soft", "Right Soft", "Call", "End Call",
            "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
            "Star", "Hash"
        };
    }

    const char *button_name(const int index) {
        return ((index >= 0) && (index < BUTTON_COUNT)) ? NAMES[index] : "";
    }
}
