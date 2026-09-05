#include "gl-context.h"

#include <drivers/graphics/context.h>

#include <memory>

namespace chimera {
    namespace {
        // A context somebody else made, current on this thread already.
        //
        // Every method here is honest about that: making it current is nothing
        // to do because it is, swapping buffers is nothing to do because there
        // is no window to swap into, and a shared context is not something an
        // embedder's single context can hand out. The machine reads its screen
        // back rather than presenting it, so none of that is missed.
        class borrowed_gl_context : public eka2l1::drivers::graphics::gl_context {
        public:
            borrowed_gl_context() {
                m_opengl_mode = mode::opengl;
            }

            bool make_current() override {
                return true;
            }

            bool clear_current() override {
                return true;
            }

            void swap_buffers() override {
            }

            void update(const std::uint32_t new_width, const std::uint32_t new_height) override {
                m_backbuffer_width = new_width;
                m_backbuffer_height = new_height;
            }

            void set_swap_interval(const std::int32_t interval) override {
                (void)interval;
            }

            bool is_headless() const override {
                return true;
            }

            std::unique_ptr<eka2l1::drivers::graphics::gl_context> create_shared_context() override {
                return nullptr;
            }
        };
    }

    void install_borrowed_gl(void *(*loader)(const char *name)) {
        eka2l1::drivers::graphics::set_gl_proc_loader(loader);

        eka2l1::drivers::graphics::set_gl_context_factory(
            [](const eka2l1::drivers::window_system_info &info, const bool stereo, const bool core) {
                (void)info;
                (void)stereo;
                (void)core;

                return std::make_unique<borrowed_gl_context>();
            });
    }
}
