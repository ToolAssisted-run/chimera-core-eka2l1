#include "null-graphics.h"

#include <drivers/itc.h>

namespace chimera {
    namespace {
        class null_graphics_driver : public eka2l1::drivers::graphics_driver {
            // Handles are only ever compared and passed back, so a counter is
            // as good as an object. Zero means failure to some callers, so
            // start at one.
            std::uint64_t next_handle_ = 1;

        public:
            null_graphics_driver()
                : eka2l1::drivers::graphics_driver(eka2l1::drivers::graphic_api::opengl) {
            }

            void run() override {
            }

            void pump() override {
            }

            void abort() override {
            }

            void dispatch(eka2l1::drivers::command &cmd) {
                // The commands that hand something back get a handle; the rest
                // are done the moment they are asked for.
                // Each creation puts a pointer to its answer in a slot of its
                // own (see drivers/itc.cpp), so the slot is part of the opcode.
                int slot = -1;

                switch (cmd.opcode_) {
                case eka2l1::drivers::graphics_driver_create_bitmap:
                case eka2l1::drivers::graphics_driver_create_shader_module:
                    slot = 2;
                    break;

                case eka2l1::drivers::graphics_driver_create_input_descriptor:
                case eka2l1::drivers::graphics_driver_create_renderbuffer:
                    slot = 3;
                    break;

                case eka2l1::drivers::graphics_driver_create_buffer:
                    slot = 4;
                    break;

                case eka2l1::drivers::graphcis_driver_create_framebuffer:
                    slot = 6;
                    break;

                case eka2l1::drivers::graphics_driver_create_texture:
                    slot = 8;
                    break;

                default:
                    break;
                }

                if (slot >= 0) {
                    if (eka2l1::drivers::handle *result
                        = reinterpret_cast<eka2l1::drivers::handle *>(cmd.data_[slot])) {
                        *result = next_handle_++;
                    }
                }

                // Some commands come with a copy of their own data, and the
                // driver owns it from the moment it is handed over. A driver
                // that draws nothing still has to let go of it: a game uploads
                // its pixels every frame, and a driver that only ignores them
                // eats the heap alive. The slot is the opcode's, again.
                int owned = -1;

                switch (cmd.opcode_) {
                case eka2l1::drivers::graphics_driver_update_bitmap:
                case eka2l1::drivers::graphics_driver_update_texture:
                case eka2l1::drivers::graphics_driver_update_buffer:
                    owned = 1;
                    break;

                case eka2l1::drivers::graphics_driver_create_input_descriptor:
                case eka2l1::drivers::graphics_driver_create_buffer:
                    owned = 0;
                    break;

                case eka2l1::drivers::graphics_driver_create_texture:
                    owned = 1;
                    break;

                default:
                    break;
                }

                if (owned >= 0) {
                    delete[] reinterpret_cast<std::uint8_t *>(cmd.data_[owned]);
                }

                finish(cmd.status_, 0);
            }

            void submit_command_list(eka2l1::drivers::command_list &cmd_list) override {
                for (std::size_t i = 0; i < cmd_list.size_; i++) {
                    dispatch(cmd_list.base_[i]);
                }

                delete[] cmd_list.base_;
            }

            void update_bitmap(eka2l1::drivers::handle, const std::size_t, const eka2l1::vec2 &,
                const eka2l1::vec2 &, const void *, const std::size_t) override {
            }

            void set_viewport(const eka2l1::rect &) override {
            }

            void update_surface(void *) override {
            }

            void update_surface_size(const eka2l1::vec2 &) override {
            }

            void set_upscale_shader(const std::string &) override {
            }

            std::string get_active_upscale_shader() const override {
                return "";
            }

            bool support_extension(const eka2l1::drivers::graphics_driver_extension) override {
                return false;
            }

            bool query_extension_value(const eka2l1::drivers::graphics_driver_extension_query, void *) override {
                return false;
            }
        };
    }

    std::unique_ptr<eka2l1::drivers::graphics_driver> make_null_graphics_driver() {
        auto driver = std::make_unique<null_graphics_driver>();

        // Nobody else will run it: the thread that submits the work is the
        // thread that finishes it.
        driver->set_driven(true);

        return driver;
    }
}
