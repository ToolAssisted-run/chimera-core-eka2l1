#pragma once

// The OpenGL the machine draws on, and where it comes from.
//
// EKA2L1 makes its own context from a window system. A core has neither: the
// context is made outside - by the host driver on a real GPU, or by whatever
// runs the native reference - and is already current on the thread the machine
// steps on. These install that context, and the loader its entry points come
// from, so the emulator's renderer runs unchanged on top of it.
namespace chimera {
    // Installs a context the emulator will "create" and a loader it will fill
    // glad from. Call before the graphics driver is made.
    void install_borrowed_gl(void *(*loader)(const char *name));
}
