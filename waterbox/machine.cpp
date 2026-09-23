#include "machine.h"
#include "audio.h"
#include "gl-context.h"
#include "input.h"
#include "null-graphics.h"
#include "host-ui.h"
#include "embedded-files.h"

#include <common/archive.h>
#include <common/path.h>
#include <config/app_settings.h>
#include <config/config.h>
#include <drivers/graphics/graphics.h>
#include <drivers/graphics/shader.h>
#include <drivers/input/common.h>
#include <drivers/itc.h>
#include <kernel/kernel.h>
#include <mem/process.h>
#include <kernel/timing.h>
#include <services/window/scheduler.h>
#include <services/window/screen.h>
#include <services/window/window.h>
#include <package/manager.h>
#include <services/applist/applist.h>
#include <utils/apacmd.h>
#include <vfs/vfs.h>
#include <loader/rom.h>
#include <system/devices.h>
#include <system/software.h>
#include <system/epoc.h>
#include <kernel/libmanager.h>

#include <algorithm>

namespace chimera {
    // EKA2L1 replaces a handful of ROM routines with its own: the screen driver
    // above all, which is the only thing that carries a direct screen access
    // game's pixels from the framebuffer chunk to the compositor. Upstream
    // finds those libraries by walking a folder. This machine has none, so it
    // answers for them out of its own binary.
    class embedded_patch_files : public eka2l1::hle::patch_file_provider {
    public:
        void list_map_files(std::vector<std::string> &names) override {
            for (unsigned int i = 0; i < PATCH_BLOB_COUNT; i++) {
                const std::string name = PATCH_BLOBS[i].name;

                if (name.size() > 4 && name.compare(name.size() - 4, 4, ".map") == 0) {
                    names.push_back(name);
                }
            }
        }

        bool read_patch_file(const std::string &name, std::vector<std::uint8_t> &data) override {
            for (unsigned int i = 0; i < PATCH_BLOB_COUNT; i++) {
                if (name == PATCH_BLOBS[i].name) {
                    data.assign(PATCH_BLOBS[i].data, PATCH_BLOBS[i].data + PATCH_BLOBS[i].size);
                    return true;
                }
            }

            return false;
        }
    };

    static embedded_patch_files PATCH_FILES;

    // The graphics driver opens its shader sources by path. Same story: this
    // machine carries them. A driver with no shaders draws nothing at all, and
    // says so only in its log.
    static bool read_embedded_shader(const std::string &path, std::string &contents) {
        const std::size_t slash = path.find_last_of("/\\");
        const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);

        for (unsigned int i = 0; i < SHADER_BLOB_COUNT; i++) {
            if (name == SHADER_BLOBS[i].name) {
                contents.assign(reinterpret_cast<const char *>(SHADER_BLOBS[i].data), SHADER_BLOBS[i].size);
                return true;
            }
        }

        return false;
    }

    machine::machine(const machine_options &options)
        : options_(options)
        , clock_(options.epoch_us, options.instructions_per_second)
        , slice_instructions_(0) {
        // The clock goes in before anything can read it: the emulator's own
        // teletimer is built on this source, and it is sampled the moment the
        // nanokernel timer starts.
        if (!options_.host_clock) {
            clock_.install();
        }

        conf_ = std::make_unique<eka2l1::config::state>();
        conf_->storage = options_.storage;

        // The processor the caller asked for (the interpreter by default, and
        // it is the reference), the scheduler must return
        // rather than sleep when nothing is runnable (this loop owns the
        // waiting), and neither the bitmap compressor nor the app scan may run
        // on a thread of its own.
        conf_->cpu_backend = options_.cpu_backend;
        conf_->cpu_load_save = false;
        conf_->fbs_enable_compression_queue = false;
        conf_->single_thread_app_scan = true;
        conf_->log_svc = options_.log_syscalls;

        // There is no network in a sandbox, and a machine told so refuses a
        // socket rather than building a loop with a thread in it.
        conf_->enable_networking = false;

        // The keypad. The emulator's keybind table translates a source code -
        // ours, since we are the one feeding it - into a Symbian scan code, so
        // this table is the whole of the machine's input mapping.
        static const std::uint32_t TARGETS[BUTTON_COUNT] = {
            eka2l1::epoc::std_key_up_arrow,
            eka2l1::epoc::std_key_down_arrow,
            eka2l1::epoc::std_key_left_arrow,
            eka2l1::epoc::std_key_right_arrow,
            eka2l1::epoc::std_key_device_3,
            eka2l1::epoc::std_key_device_0,
            eka2l1::epoc::std_key_device_1,
            eka2l1::epoc::std_key_application_0,
            eka2l1::epoc::std_key_application_1,
            '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
            '*', '#'
        };

        conf_->keybinds.keybinds.clear();

        for (int i = 0; i < BUTTON_COUNT; i++) {
            eka2l1::config::keybind bind;

            bind.source.type = eka2l1::config::KEYBIND_TYPE_KEY;
            bind.source.data.keycode = static_cast<std::uint32_t>(i);
            bind.target = TARGETS[i];

            conf_->keybinds.keybinds.push_back(bind);
        }

        buttons_.assign(BUTTON_COUNT, false);

        settings_ = std::make_unique<eka2l1::config::app_settings>(conf_.get());

        eka2l1::system_create_components comp;
        comp.graphics_ = nullptr;
        comp.audio_ = nullptr;
        comp.conf_ = conf_.get();
        comp.settings_ = settings_.get();
        comp.cache_root_ = options_.storage;

        sys_ = std::make_unique<eka2l1::system>(comp);

        sys_->set_cpu_ticks_callback([this](const std::uint32_t instructions) {
            slice_instructions_ += instructions;
            clock_.spend_instructions(instructions);
        });
    }

    machine::~machine() {
        sys_.reset();
        virtual_clock::uninstall();
    }

    void machine::startup() {
        sys_->startup();

        // Nobody else may move this machine's time. The nanokernel timer would
        // otherwise start a thread that sleeps on the host clock and fires the
        // kernel's timers off it.
        sys_->get_ntimer()->set_driven(true);

        // reset() is what starts the timer's own measurement - and, in driven
        // mode, all it does. Without it the timer has never been told when
        // "now" began and reads the same instant forever, so no event it holds
        // ever comes due. set_device() calls it again, which costs nothing.
        sys_->get_ntimer()->reset();
    }

    void machine::start_null_graphics() {
        if (gdriver_) {
            return;
        }

        gdriver_ = make_null_graphics_driver();
        gdriver_->set_display_hook([]() {});
        sys_->set_graphics_driver(gdriver_.get());
    }

    void machine::start_graphics(void *(*loader)(const char *name)) {
        install_borrowed_gl(loader);
        eka2l1::drivers::set_resource_provider(read_embedded_shader);

        eka2l1::drivers::window_system_info info;
        gdriver_ = eka2l1::drivers::create_graphics_driver(eka2l1::drivers::graphic_api::opengl, info);

        if (!gdriver_) {
            return;
        }

        // Nothing else will run its command lists: this loop does, between the
        // machine's own steps.
        gdriver_->set_driven(true);

        // The driver calls this after every present. There is no window to
        // present into and nothing to poll, but an unset std::function throws
        // when called, and the machine does present.
        gdriver_->set_display_hook([]() {});

        sys_->set_graphics_driver(gdriver_.get());
    }

    void machine::start_audio() {
        adriver_ = std::make_shared<audio_sink>(options_.sample_rate);
        sys_->set_audio_driver(adriver_.get());
    }

    void machine::render_audio(std::int16_t *out, const std::size_t frames) {
        if (!adriver_) {
            std::fill(out, out + frames * 2, static_cast<std::int16_t>(0));
            return;
        }

        adriver_->render(out, frames);
    }

    void machine::set_button(const int index, const bool held) {
        if ((index < 0) || (index >= BUTTON_COUNT) || (buttons_[index] == held)) {
            return;
        }

        buttons_[index] = held;

        eka2l1::kernel_system *kern = sys_->get_kernel_system();

        if (!kern) {
            return;
        }

        eka2l1::window_server *winserv = reinterpret_cast<eka2l1::window_server *>(
            kern->get_by_name<eka2l1::service::server>(
                eka2l1::get_winserv_name_by_epocver(sys_->get_symbian_version_use())));

        if (!winserv) {
            return;
        }

        eka2l1::drivers::input_event event;

        event.type_ = eka2l1::drivers::input_event_type::key;
        event.key_.state_ = held ? eka2l1::drivers::key_state::pressed : eka2l1::drivers::key_state::released;
        event.key_.code_ = index;

        winserv->queue_input_from_driver(event);
    }

    // The panel's own memory, as a picture.
    //
    // A game drawing straight into the framebuffer has already written every
    // pixel there, in whatever depth the panel reports. Reading it takes no
    // graphics driver and no compositor: it is machine memory, so the picture
    // is the same in every flavor and travels in the machine's savestates.
    /* One 12-bit pixel, 0x0RGB, into 0xFFRRGGBB - red in the high nibble, as
     * Symbian's EColor4K has it and as the emulator's own bitmap decoder reads
     * it (services/src/fbs/impls/bitmap.cpp). Pulled out of the loop so the
     * channel order is something a test states rather than something only a
     * picture reveals. See colour_check_12bpp(). */
    std::uint32_t unpack12(const std::uint16_t v)
    {
        const std::uint32_t r = ((v >> 8) & 0xF) * 17;
        const std::uint32_t g = ((v >> 4) & 0xF) * 17;
        const std::uint32_t b = (v & 0xF) * 17;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    /* One pixel as the compositor's readback leaves it - the bytes R, G, B, A
     * in memory (GL_RGBA, unsigned bytes) - into 0xFFRRGGBB, the order every
     * other path here produces and the frontend reads. Taken as a word on a
     * little-endian host that is 0xAABBGGRR: red and blue exchanged, which is
     * what chimera#130 was. */
    std::uint32_t rgba_bytes_to_argb(const std::uint32_t word)
    {
        return 0xFF000000u | ((word & 0xFFu) << 16) | (word & 0xFF00u) | ((word >> 16) & 0xFFu);
    }

    /* Known values in, known colours out, for both paths a picture takes.
     * Pure red, green and blue each live in one channel, so a reordering
     * cannot pass this.
     *
     * chimera#130's history is why both are here. The reporter's Sonic was
     * drawn red where he is blue, and the first fix exchanged the channels of
     * the 12-bit DIRECT path - which that game never takes: its picture is the
     * window server's, composed and read back as RGBA bytes, and the colours
     * in the before and after screenshots are identical, value for value. That
     * fix made the direct path wrong (MotoGP draws there after its first 433
     * frames) and left the compositor as it was. */
    bool colour_check_12bpp(void)
    {
        struct { std::uint16_t in; std::uint32_t want; const char *what; } cases12[] = {
            { 0x0F00, 0xFFFF0000u, "12-bit 0x0F00 is red" },
            { 0x00F0, 0xFF00FF00u, "12-bit 0x00F0 is green" },
            { 0x000F, 0xFF0000FFu, "12-bit 0x000F is blue" },
            { 0x0FFF, 0xFFFFFFFFu, "12-bit 0x0FFF is white" },
            { 0x0000, 0xFF000000u, "12-bit 0x0000 is black" },
        };
        /* bytes R,G,B,A read as a little-endian word */
        struct { std::uint32_t in; std::uint32_t want; const char *what; } cases32[] = {
            { 0xFF0000FFu, 0xFFFF0000u, "RGBA bytes FF 00 00 FF are red" },
            { 0xFF00FF00u, 0xFF00FF00u, "RGBA bytes 00 FF 00 FF are green" },
            { 0xFFFF0000u, 0xFF0000FFu, "RGBA bytes 00 00 FF FF are blue" },
            { 0xFFEE4400u, 0xFF0044EEu, "Sonic's blue, as the compositor hands it over" },
        };
        bool ok = true;
        for (const auto &c : cases12) {
            const std::uint32_t got = unpack12(c.in);
            if (got != c.want) {
                std::fprintf(stderr, "colour: %s - got %08x want %08x\n", c.what, got, c.want);
                ok = false;
            }
        }
        for (const auto &c : cases32) {
            const std::uint32_t got = rgba_bytes_to_argb(c.in);
            if (got != c.want) {
                std::fprintf(stderr, "colour: %s - got %08x want %08x\n", c.what, got, c.want);
                ok = false;
            }
        }
        return ok;
    }

    static bool read_framebuffer(eka2l1::epoc::screen *scr, std::vector<std::uint32_t> &out,
        int &width, int &height) {
        if (!scr->screen_buffer_chunk) {
            return false;
        }

        const eka2l1::epoc::config::screen_mode &mode = scr->current_mode();

        // A rotated panel is laid out to suit the rotation. Leave those to the
        // compositor, which already knows how to turn them the right way up.
        if ((mode.rotation != 0) || (mode.size.x <= 0) || (mode.size.y <= 0)) {
            return false;
        }

        const std::uint32_t bpp = eka2l1::epoc::get_bpp_from_display_mode(scr->dsa_disp_mode);
        const std::uint32_t pitch = scr->screen_buffer_byte_width(scr->dsa_disp_mode);
        const std::uint8_t *base = scr->screen_buffer_ptr();

        if (!base) {
            return false;
        }


        width = mode.size.x;
        height = mode.size.y;
        out.resize(static_cast<std::size_t>(width) * height);

        for (int y = 0; y < height; y++) {
            const std::uint8_t *row = base + static_cast<std::size_t>(y) * pitch;

            for (int x = 0; x < width; x++) {
                std::uint32_t r = 0;
                std::uint32_t g = 0;
                std::uint32_t b = 0;

                switch (bpp) {
                case 12: {
                    // 0x0RGB, one nibble each, spread over the full range
                    // (unpack12, and the history there).
                    const std::uint32_t argb = unpack12(
                        *reinterpret_cast<const std::uint16_t *>(row + x * 2));
                    r = (argb >> 16) & 0xFF;
                    g = (argb >> 8) & 0xFF;
                    b = argb & 0xFF;
                    break;
                }

                case 16: {
                    // 565. UNVERIFIED: nothing on this machine draws in this
                    // mode, so unlike the 12-bit case above this order has
                    // never been checked against a picture whose colours are
                    // known. If a game ever comes out with its reds and blues
                    // exchanged, look here first and check it the same way.
                    const std::uint16_t v = *reinterpret_cast<const std::uint16_t *>(row + x * 2);
                    r = ((v >> 11) & 0x1F) * 255 / 31;
                    g = ((v >> 5) & 0x3F) * 255 / 63;
                    b = (v & 0x1F) * 255 / 31;
                    break;
                }

                case 24:
                    b = row[x * 3];
                    g = row[x * 3 + 1];
                    r = row[x * 3 + 2];
                    break;

                case 32: {
                    const std::uint32_t v = *reinterpret_cast<const std::uint32_t *>(row + x * 4);
                    r = (v >> 16) & 0xFF;
                    g = (v >> 8) & 0xFF;
                    b = v & 0xFF;
                    break;
                }

                default:
                    // Palette and grayscale panels: nothing this port has met.
                    return false;
                }

                out[static_cast<std::size_t>(y) * width + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }

        return true;
    }

    bool machine::read_screen(std::vector<std::uint32_t> &out, int &width, int &height) {
        eka2l1::kernel_system *kern = sys_->get_kernel_system();

        if (!kern) {
            return false;
        }

        eka2l1::window_server *winserv = reinterpret_cast<eka2l1::window_server *>(
            kern->get_by_name<eka2l1::service::server>(
                eka2l1::get_winserv_name_by_epocver(sys_->get_symbian_version_use())));

        if (!winserv) {
            return false;
        }

        eka2l1::epoc::screen *scr = winserv->get_screen(0);

        if (!scr) {
            return false;
        }

        // A direct screen access client wrote its picture into the panel
        // itself. That memory is the machine's, so read it there: it needs no
        // graphics driver, which is the only way a sandboxed core gets a
        // picture at all.
        if ((scr->dsa_active_count() > 0) && read_framebuffer(scr, out, width, height)) {
            return true;
        }


        if (!gdriver_) {
            return false;
        }

        // Otherwise the picture is the window tree, and somebody has to
        // compose it. Nothing else will ask: the window server redraws on its
        // own schedule for a display that is watching, and here the only
        // watcher is whoever called this.
        winserv->get_anim_scheduler()->scan_for_redraw(gdriver_.get(), 0, true);
        gdriver_->pump();

        if (!scr->screen_texture) {
            return false;
        }

        const eka2l1::vec2 size = scr->size();

        if ((size.x <= 0) || (size.y <= 0)) {
            return false;
        }

        width = size.x;
        height = size.y;
        out.resize(static_cast<std::size_t>(width) * height);

        if (!eka2l1::drivers::read_bitmap(gdriver_.get(), scr->screen_texture, eka2l1::point(0, 0),
                eka2l1::object_size(width, height), 32, reinterpret_cast<std::uint8_t *>(out.data()))) {
            return false;
        }

        // RGBA bytes, where the frontend reads 0xFFRRGGBB (rgba_bytes_to_argb).
        for (std::uint32_t &pixel : out) {
            pixel = rgba_bytes_to_argb(pixel);
        }

        return true;
    }

    static eka2l1::applist_server *app_list(eka2l1::system *sys) {
        eka2l1::kernel_system *kern = sys->get_kernel_system();

        if (!kern) {
            return nullptr;
        }

        return reinterpret_cast<eka2l1::applist_server *>(
            kern->get_by_name<eka2l1::service::server>(
                eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));
    }

    bool machine::launch_app(const std::uint32_t uid) {
        eka2l1::applist_server *applist = app_list(sys_.get());

        if (!applist) {
            return false;
        }

        eka2l1::apa_app_registry *registry = applist->get_registration(uid);

        if (!registry) {
            return false;
        }

        eka2l1::epoc::apa::command_line cmdline;
        cmdline.launch_cmd_ = eka2l1::epoc::apa::command_create;

        if (launched_uid_ == uid) {
            // Already running. Starting it twice would be a second process,
            // not a restart.
            return true;
        }

        eka2l1::kernel::uid thread_id = 0;

        if (!applist->launch_app(*registry, cmdline, &thread_id, nullptr)) {
            return false;
        }

        launched_uid_ = uid;

        // Whose memory the bus reads. Taken here rather than looked up on every
        // access: by the time anybody asks, the thread that happens to be
        // current is as likely to be the window server's as the game's.
        eka2l1::kernel_system *kern = sys_->get_kernel_system();
        eka2l1::kernel::thread *starter = kern ? kern->get_by_id<eka2l1::kernel::thread>(thread_id) : nullptr;
        eka2l1::kernel::process *owner = starter ? starter->owning_process() : nullptr;

        if (owner && owner->get_mem_model()) {
            game_asid_ = static_cast<std::int32_t>(owner->get_mem_model()->address_space_id());
        }

        return true;
    }

    // One page's worth of translation, kept: a search walks addresses in order,
    // so all but one byte in four thousand is answered without touching the
    // page tables.
    std::uint8_t *machine::resolve_user(const std::uint32_t addr) {
        constexpr std::uint32_t PAGE_MASK = 0xFFFFF000u;

        const std::uint32_t page = addr & PAGE_MASK;

        if ((page == cached_page_) && cached_host_) {
            return cached_host_ + (addr & ~PAGE_MASK);
        }

        eka2l1::memory_system *mem = sys_->get_memory_system();

        // The lowest page is Symbian's null trap and is never real memory.
        if (!mem || !page) {
            return nullptr;
        }

        // The page's own base, so the whole page is served from one lookup. A
        // page nothing is mapped at has no host address at all.
        std::uint8_t *host = reinterpret_cast<std::uint8_t *>(mem->get_real_pointer(page, game_asid_));

        if (!host) {
            cached_page_ = 0xFFFFFFFF;
            cached_host_ = nullptr;
            return nullptr;
        }

        cached_page_ = page;
        cached_host_ = host;

        return host + (addr & ~PAGE_MASK);
    }

    std::uint8_t machine::peek_user(const std::uint32_t addr) {
        const std::uint8_t *at = resolve_user(addr);
        return at ? *at : 0;
    }

    void machine::poke_user(const std::uint32_t addr, const std::uint8_t value) {
        if (std::uint8_t *at = resolve_user(addr)) {
            *at = value;
        }
    }

    void machine::remember_apps() {
        eka2l1::applist_server *applist = app_list(sys_.get());

        apps_before_install_.clear();

        if (!applist) {
            return;
        }

        for (const eka2l1::apa_app_registry &reg : applist->get_registerations()) {
            apps_before_install_.push_back(reg.mandatory_info.uid);
        }
    }

    std::uint32_t machine::launch_installed_app() {
        eka2l1::applist_server *applist = app_list(sys_.get());

        if (!applist) {
            return 0;
        }

        // Which application is the project's? The one the machine did not have
        // before it was given the project's file. The registration itself
        // cannot be asked: on this EKA1 device every ROM application claims to
        // have landed on drive C, whatever drive it is actually on.
        //
        // Lowest UID of the new ones, so a card holding more than one always
        // starts the same one.
        std::uint32_t chosen = 0;

        for (const eka2l1::apa_app_registry &reg : applist->get_registerations()) {
            const std::uint32_t uid = reg.mandatory_info.uid;

            if (std::find(apps_before_install_.begin(), apps_before_install_.end(), uid)
                != apps_before_install_.end()) {
                continue;
            }

            if ((chosen == 0) || (uid < chosen)) {
                chosen = uid;
            }
        }

        if ((chosen == 0) || !launch_app(chosen)) {
            return 0;
        }

        return chosen;
    }

    // Whether the machine still has a thread called blzinstapp. Its own
    // "Exiting..." is the only thing that says the unpack is over.
    static bool installer_alive(eka2l1::kernel_system *kern) {
        bool alive = false;

        for (auto &object : kern->get_thread_list()) {
            eka2l1::kernel::thread *thread = reinterpret_cast<eka2l1::kernel::thread *>(object.get());

            if (thread && (thread->name().find("blzinstapp") != std::string::npos)
                && (thread->current_state() != eka2l1::kernel::thread_state::stop)) {
                alive = true;
            }
        }

        return alive;
    }

    bool machine::install_blz(const std::string &blz_path, const std::string &installer_path) {
        // Where the application looks: the root of the memory card, and only
        // for *.blz. The name it shows is the file's, so keep the caller's.
        std::string leaf = blz_path;
        const std::size_t slash = leaf.find_last_of("/\\");

        if (slash != std::string::npos) {
            leaf = leaf.substr(slash + 1);
        }

        if (!put_file("E:\\" + leaf, blz_path)) {
            return false;
        }

        // What the machine has before any of this, so the installer and then
        // the game can each be told apart from the phone's own applications.
        remember_apps();

        // The application itself, onto drive C, and the application list has to
        // be told the drive changed or it will never see it.
        if (install_package(installer_path) != 0) {
            return false;
        }

        sys_->get_io_system()->announce_drive(drive_c, eka2l1::drive_action_mount);

        const std::uint32_t installer_uid = launch_installed_app();

        if (installer_uid == 0) {
            return false;
        }

        // Its menu, in the two keys it takes: the left soft key opens Options,
        // and the left soft key again chooses Install, which is the first item.
        // The frames are the application's own pace - it lists the card before
        // it will answer anything - and the machine runs at its own speed here,
        // so they are the same frames every time.
        constexpr std::uint64_t FRAME_US = 1000000ull / 60ull;
        constexpr int OPTIONS_FRAME = 400;
        constexpr int INSTALL_FRAME = 500;
        constexpr int GIVE_UP_FRAME = 20000;

        eka2l1::kernel_system *kern = sys_->get_kernel_system();

        for (int frame = 0; frame < GIVE_UP_FRAME; frame++) {
            if ((frame == OPTIONS_FRAME) || (frame == INSTALL_FRAME)) {
                set_button(BUTTON_SOFT_LEFT, true);
            } else if ((frame == OPTIONS_FRAME + 20) || (frame == INSTALL_FRAME + 20)) {
                set_button(BUTTON_SOFT_LEFT, false);
            }

            run_for_us(FRAME_US);

            // The application says goodbye by exiting, and nothing else it
            // does says it is finished.
            if ((frame > INSTALL_FRAME + 20) && kern && !installer_alive(kern)) {
                break;
            }
        }

        // Whatever it wrote is on the card, and the application list has not
        // been told about that either.
        sys_->get_io_system()->announce_drive(drive_e, eka2l1::drive_action_mount);

        // The installer is no longer the project's application: the game it
        // unpacked is. Forget it, so the game is what gets started.
        apps_before_install_.push_back(installer_uid);
        launched_uid_ = 0;

        return true;
    }

    bool machine::put_file(const std::string &machine_path, const std::string &host_path) {
        std::FILE *source = std::fopen(host_path.c_str(), "rb");

        if (!source) {
            return false;
        }

        std::vector<std::uint8_t> bytes;
        std::uint8_t buffer[65536];
        std::size_t got = 0;

        while ((got = std::fread(buffer, 1, sizeof(buffer), source)) > 0) {
            bytes.insert(bytes.end(), buffer, buffer + got);
        }

        std::fclose(source);

        eka2l1::symfile target = sys_->get_io_system()->open_file(
            eka2l1::common::utf8_to_ucs2(machine_path), WRITE_MODE | BIN_MODE);

        if (!target) {
            return false;
        }

        const bool written = (target->write_file(bytes.data(), static_cast<std::uint32_t>(bytes.size()), 1)
            == static_cast<std::size_t>(bytes.size()));

        target->close();
        return written;
    }

    int machine::install_card(const std::string &archive_path) {
        std::vector<eka2l1::common::archive_entry_info> entries;

        if (!eka2l1::common::list_archive(archive_path, entries)) {
            return 0;
        }

        // The card's root is whatever holds "System": a dump is usually one
        // folder named after the game, and sometimes the card itself.
        std::string prefix;
        bool found_root = false;

        for (const auto &entry : entries) {
            const std::string lowered = eka2l1::common::lowercase_string(entry.path);
            const std::size_t at = lowered.find("system/");

            if ((at == std::string::npos) || (at != 0 && lowered[at - 1] != '/')) {
                continue;
            }

            prefix = entry.path.substr(0, at);
            found_root = true;
            break;
        }

        if (!found_root) {
            return 0;
        }

        eka2l1::io_system *io = sys_->get_io_system();
        int written = 0;

        for (const auto &entry : entries) {
            if (entry.is_directory || (entry.path.compare(0, prefix.size(), prefix) != 0)) {
                continue;
            }

            const std::string relative = entry.path.substr(prefix.size());

            if (relative.empty()) {
                continue;
            }

            std::vector<char> content;

            if (!eka2l1::common::read_archive_entry(archive_path, entry.path, content)) {
                continue;
            }

            // Symbian spells its paths with backslashes, and the card goes on
            // the removable drive, which is where a game card is.
            std::string target = "E:\\" + relative;

            for (char &c : target) {
                if (c == '/') {
                    c = '\\';
                }
            }

            const std::u16string wide = eka2l1::common::utf8_to_ucs2(target);

            io->create_directories(eka2l1::common::utf8_to_ucs2(eka2l1::file_directory(target)));

            std::unique_ptr<eka2l1::file> out = io->open_file(wide, WRITE_MODE | BIN_MODE);

            if (!out) {
                continue;
            }

            if (!content.empty()) {
                out->write_file(content.data(), 1, static_cast<std::uint32_t>(content.size()));
            }

            out->close();
            written++;
        }

        return written;
    }

    int machine::install_package(const std::string &path) {
        return sys_->install_package(eka2l1::common::utf8_to_ucs2(path), drive_c);
    }

    bool machine::add_device_from_rom(const std::string &rom_path) {
        eka2l1::symfile rom_file = eka2l1::physical_file_proxy(rom_path, READ_MODE | BIN_MODE);

        if (!rom_file) {
            return false;
        }

        eka2l1::ro_file_stream rom_stream(rom_file.get());
        std::optional<eka2l1::loader::rom> parsed = eka2l1::loader::load_rom(
            reinterpret_cast<eka2l1::common::ro_stream *>(&rom_stream));

        if (!parsed) {
            return false;
        }

        std::string manufacturer;
        std::string firmcode;
        std::string model;
        epocver ver = epocver::epoc94;

        if (!eka2l1::loader::determine_device_from_rom(parsed.value(),
                reinterpret_cast<eka2l1::common::ro_stream *>(&rom_stream), manufacturer, firmcode, model, ver)) {
            return false;
        }

        if (sys_->get_device_manager()->add_new_device(firmcode, model, manufacturer, ver, 0)
            != eka2l1::add_device_none) {
            return false;
        }

        // And it is that file, not one under a storage layout nobody built.
        sys_->set_rom_path(rom_path);

        return true;
    }

    std::size_t machine::device_count() const {
        return sys_->get_device_manager()->total();
    }

    bool machine::set_device(const std::size_t index) {
        return sys_->set_device(static_cast<std::uint8_t>(index));
    }

    void machine::boot() {
        if (options_.in_memory_drives) {
            // Drive Z is the ROM's, always: the ROM carries its own filesystem
            // and the ROM filesystem is the only one that will take a volume of
            // rom media. The writable drives are the caller's.
            sys_->mount(drive_z, drive_media::rom,
                eka2l1::add_path(options_.storage, "/drives/z/"),
                io_attrib_internal | io_attrib_write_protected);
        } else {
            mount_host_drives();
        }

        // Before the user side comes up: initialising it is what loads them.
        sys_->get_lib_manager()->set_patch_file_provider(&PATCH_FILES);

        sys_->initialize_user_parties();

        eka2l1::manager::packages *packages = sys_->get_packages();
        packages->load_registries();
        packages->migrate_legacy_registries();
    }

    void machine::mount_host_drives() {
        // Drive Z is the ROM's own filesystem and can only be mounted once the
        // device has been set, because setting the device is what loads the ROM.
        sys_->mount(drive_c, drive_media::physical,
            eka2l1::add_path(options_.storage, "/drives/c/"), io_attrib_internal);
        sys_->mount(drive_d, drive_media::physical,
            eka2l1::add_path(options_.storage, "/drives/d/"), io_attrib_internal);
        sys_->mount(drive_e, drive_media::physical,
            eka2l1::add_path(options_.storage, "/drives/e/"), io_attrib_removeable);
        sys_->mount(drive_z, drive_media::rom,
            eka2l1::add_path(options_.storage, "/drives/z/"),
            io_attrib_internal | io_attrib_write_protected);
    }

    std::uint64_t machine::run_for_us(const std::uint64_t us) {
        const std::uint64_t target = clock_.now_us() + us;
        std::uint64_t loops = 0;

        eka2l1::ntimer *timing = sys_->get_ntimer();

        // Without a device there is no memory model, and with no memory model
        // there is nothing to schedule: the emulator's loop would switch to a
        // thread that cannot exist. Time still passes for such a machine, and
        // its timers still come due, which is exactly what an empty machine is
        // useful for testing.
        const bool runnable = (sys_->get_memory_system() != nullptr);

        while (clock_.now_us() < target) {
            slice_instructions_ = 0;

            if (runnable) {
                sys_->loop();
                loops++;
            }

            // Dialogs raised during the slice are answered here, outside the
            // kernel lock the caller was holding.
            pump_host_ui();

            // And whatever the machine asked to be drawn.
            if (gdriver_) {
                gdriver_->pump();
            }

            // Fire whatever the instructions just bought, and learn when the
            // next deadline is.
            const std::optional<std::uint64_t> next = timing->advance();

            if (slice_instructions_ != 0) {
                continue;
            }

            // Nothing ran: every thread is waiting on something. Time still
            // passes, so buy it outright - up to the next deadline, and never
            // past the end of the slice we were asked for.
            const std::uint64_t remaining = target - clock_.now_us();
            const std::uint64_t jump = next.has_value() ? std::max<std::uint64_t>(next.value(), 1) : remaining;

            clock_.advance_us(std::min(jump, remaining));
        }

        return loops;
    }
}
