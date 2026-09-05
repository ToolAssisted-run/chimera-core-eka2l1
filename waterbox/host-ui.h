#pragma once

namespace chimera {
    // Answers the dialogs the emulator raised since the last call. Dialog
    // callbacks take the kernel lock, and the caller that raised the dialog is
    // holding it, so a core answers between steps and never inside the service
    // call. The step loop calls this once per slice.
    void pump_host_ui();
}
