/* gl-osmesa.c - the OpenGL a sandboxed machine draws on.
 *
 * There is no GPU here and no host driver to borrow one from, so the machine
 * gets a whole OpenGL of its own: Mesa's softpipe, compiled into this core.
 * It is plain C with no JIT and no dispatch on host CPU features, so what it
 * draws is decided entirely by code we compiled - which is the only kind of
 * renderer a core that has to replay a movie can use.
 *
 * Guest only. The native reference borrows the host's context instead
 * (gl-egl.c), which is why the two flavors' PICTURES are not compared when
 * something draws through the window server - only the machine is.
 */
#include <GL/osmesa.h>

#include <stdio.h>
#include <stdlib.h>

static OSMesaContext g_ctx;
static void *g_buffer;

/* Makes the context and leaves it current on this thread, the way the
 * emulator's renderer expects to find one. Answers 0 when Mesa refuses -
 * usually for want of heap, which is the first thing to check. */
int chimera_osmesa_start(int width, int height)
{
	static const int attribs[] = {
		OSMESA_FORMAT, OSMESA_RGBA,
		OSMESA_DEPTH_BITS, 24,
		OSMESA_STENCIL_BITS, 8,
		OSMESA_ACCUM_BITS, 0,
		OSMESA_PROFILE, OSMESA_COMPAT_PROFILE,
		OSMESA_CONTEXT_MAJOR_VERSION, 3,
		OSMESA_CONTEXT_MINOR_VERSION, 3,
		0
	};

	if (g_ctx) return 1;

	g_ctx = OSMesaCreateContextAttribs(attribs, NULL);
	if (!g_ctx) return 0;

	/* The default framebuffer. Everything the window server composes goes to
	 * textures it made itself; this is only what GL calls "the window", and it
	 * has to be at least as large as anything bound to framebuffer zero. */
	g_buffer = calloc((size_t)width * height, 4);
	if (!g_buffer) return 0;

	if (!OSMesaMakeCurrent(g_ctx, g_buffer, GL_UNSIGNED_BYTE, width, height)) return 0;

	/* Rows the way every other renderer here counts them. */
	OSMesaPixelStore(OSMESA_Y_UP, 0);
	return 1;
}

void *chimera_osmesa_proc(const char *name)
{
	return (void *)OSMesaGetProcAddress(name);
}
