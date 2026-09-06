#pragma once

#include <drivers/graphics/graphics.h>

#include <memory>

namespace chimera {
    // A graphics driver with nowhere to draw.
    //
    // The window server assumes it has one. Not for the picture - a game that
    // draws through direct screen access paints the panel itself, and this core
    // reads the panel - but for the bookkeeping: a canvas that needs resizing
    // asks the driver for a bitmap, text asks it for a glyph atlas, and every
    // one of those calls goes straight through a pointer. Without a driver a
    // machine that runs a user-interface application does not draw nothing, it
    // dies.
    //
    // So the sandbox gets this: a driver that accepts every command, executes
    // none, and answers the ones that ask for a handle with a number of its
    // own. The machine draws into it exactly as it would into a real one, and
    // the pixels go nowhere.
    std::unique_ptr<eka2l1::drivers::graphics_driver> make_null_graphics_driver();
}
