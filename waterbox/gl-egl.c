/* A headless OpenGL context for the native reference.
 *
 * The sandbox gets its GL through the bridge, on a context the host driver
 * makes. The native reference has no such host, so it makes one here: EGL's
 * surfaceless platform, which Mesa provides with or without a GPU. Nothing
 * about the machine depends on which - the machine reads its screen back
 * rather than presenting it - but a context there must be, because the
 * emulator's renderer is a real OpenGL renderer.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <stdio.h>
#include <string.h>

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;

int chimera_egl_make_context(char *err, int errlen)
{
	/* The surfaceless platform: Mesa provides it with or without a GPU, and
	 * it is the only display that does not want a window system. */
	s_display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);

	if (s_display == EGL_NO_DISPLAY) {
		snprintf(err, errlen, "no surfaceless EGL display");
		return 0;
	}

	if (!eglInitialize(s_display, NULL, NULL)) {
		snprintf(err, errlen, "eglInitialize failed");
		return 0;
	}

	if (!eglBindAPI(EGL_OPENGL_API)) {
		snprintf(err, errlen, "no desktop OpenGL through EGL");
		return 0;
	}

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_NONE
	};

	EGLConfig config;
	EGLint config_count = 0;

	if (!eglChooseConfig(s_display, config_attribs, &config, 1, &config_count) || (config_count < 1)) {
		snprintf(err, errlen, "no EGL config with desktop OpenGL");
		return 0;
	}

	/* A pbuffer, even though nothing will ever look at it: a renderer draws
	 * into framebuffer 0 unless told otherwise, and a context made current
	 * with no surface has no framebuffer 0 at all. The machine composites into
	 * its own textures and the screen is read back from one of them, so the
	 * size only has to be legal. */
	static const EGLint surface_attribs[] = { EGL_WIDTH, 1024, EGL_HEIGHT, 1024, EGL_NONE };
	EGLSurface surface = eglCreatePbufferSurface(s_display, config, surface_attribs);

	if (surface == EGL_NO_SURFACE) {
		snprintf(err, errlen, "eglCreatePbufferSurface failed");
		return 0;
	}

	static const EGLint core45[] = {
		EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
		EGL_NONE
	};
	static const EGLint any33[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_NONE
	};

	s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, core45);

	if (s_context == EGL_NO_CONTEXT) {
		s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, any33);
	}

	if (s_context == EGL_NO_CONTEXT) {
		snprintf(err, errlen, "eglCreateContext failed");
		return 0;
	}

	if (!eglMakeCurrent(s_display, surface, surface, s_context)) {
		snprintf(err, errlen, "eglMakeCurrent failed");
		return 0;
	}

	return 1;
}

void *chimera_egl_proc(const char *name)
{
	return (void *)eglGetProcAddress(name);
}
