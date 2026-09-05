// The four host-side functions the emulator declares but never defines: the
// frontend is expected to supply them (Qt has dialog_driver.cpp and
// common_library_plugin.cpp). A core has no user to ask and no browser to open,
// so each answers the way a machine with nobody at the keyboard would: the
// input view never opens, the yes/no dialog is declined, the browser refuses.
//
// Answering matters more than it looks. A Symbian app that asks and is told
// "no" carries on; one left waiting hangs the machine. But the answer cannot be
// given where the question was asked: `show_yes_no_dialog` is called from a
// service handler with the kernel lock held, and completing a request takes
// that same non-recursive lock. So the answer is queued and delivered from
// pump_host_ui(), which the step loop calls between slices.
#include "host-ui.h"

#include <common/applauncher.h>
#include <drivers/ui/input_dialog.h>

#include <utility>
#include <vector>

namespace {
    std::vector<eka2l1::drivers::ui::yes_no_dialog_complete_callback> g_pending_dialogs;
}

namespace chimera {
    void pump_host_ui() {
        if (g_pending_dialogs.empty()) {
            return;
        }

        // A callback may raise another dialog, so the queue is taken away
        // before any of it runs.
        std::vector<eka2l1::drivers::ui::yes_no_dialog_complete_callback> answering;
        answering.swap(g_pending_dialogs);

        for (auto &callback : answering) {
            // Symbian's RNotifier::Notify answers with the index of the button
            // pressed. Index 1 is the second button, which is where a two
            // button dialog puts the refusal.
            callback(1);
        }
    }
}

namespace eka2l1::drivers::ui {
    bool open_input_view(const std::u16string &initial_text, const int max_len,
        input_dialog_complete_callback complete_callback) {
        (void)initial_text;
        (void)max_len;
        (void)complete_callback;

        // False is "the view did not open", which every caller handles.
        return false;
    }

    void close_input_view() {
    }

    void show_yes_no_dialog(const std::u16string &text, const std::u16string &button1_text,
        const std::u16string &button2_text, yes_no_dialog_complete_callback complete_callback) {
        (void)text;
        (void)button1_text;
        (void)button2_text;

        if (complete_callback) {
            g_pending_dialogs.push_back(std::move(complete_callback));
        }
    }
}

namespace eka2l1::common {
    bool launch_browser(const std::string &url) {
        (void)url;
        return false;
    }
}
