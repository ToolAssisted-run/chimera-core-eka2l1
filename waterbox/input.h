#pragma once

// The N-Gage's keypad, as a core sees it.
//
// A chimera host sends button indices; the machine wants Symbian scan codes.
// In between is the emulator's own keybind table, whose "source" numbering is
// whatever the embedder feeds it - so the indices below ARE that numbering,
// and the table below is the whole translation.
namespace chimera {
    enum button {
        BUTTON_UP,
        BUTTON_DOWN,
        BUTTON_LEFT,
        BUTTON_RIGHT,
        BUTTON_SELECT,
        BUTTON_SOFT_LEFT,
        BUTTON_SOFT_RIGHT,
        BUTTON_CALL,
        BUTTON_END_CALL,
        BUTTON_0,
        BUTTON_1,
        BUTTON_2,
        BUTTON_3,
        BUTTON_4,
        BUTTON_5,
        BUTTON_6,
        BUTTON_7,
        BUTTON_8,
        BUTTON_9,
        BUTTON_STAR,
        BUTTON_HASH,
        BUTTON_COUNT
    };

    // The name each button answers to in a movie and in the frontend.
    const char *button_name(const int index);
}
