#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/glx/GrGLMakeGLXInterface.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#include "modules/svg/include/SkSVGDOM.h"

#include <GL/glx.h>
#include <X11/Xlib.h>

#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

int main(int argc, char **argv) {
	bool use_gpu = false;
	const char *filename = nullptr;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--gpu") == 0) use_gpu = true;
		else filename = argv[i];
	}

	if (!filename) {
		fprintf(stderr, "usage: %s [--gpu] file.svg\n", argv[0]);
		return EXIT_FAILURE;
	}

	// -------------------------------------------------------------------------
	// Load + parse SVG
	// -------------------------------------------------------------------------

	double load_begin = now_ms();

	auto stream = SkStream::MakeFromFile(filename);

	if (!stream) {
		fprintf(stderr, "open failed: %s\n", filename);
		return EXIT_FAILURE;
	}

	auto dom = SkSVGDOM::Builder()
		.setFontManager(SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType()))
		.make(*stream);

	double load_end = now_ms();

	if (!dom) {
		fprintf(stderr, "parse failed: %s\n", filename);
		return EXIT_FAILURE;
	}

	// containerSize() returns intrinsic size before setContainerSize() is called.
	// Falls back to 512x512 for viewBox-only or %-based roots.
	const SkSize sz = dom->containerSize();
	int width = (sz.width() > 0) ? (int)sz.width() : 512;
	int height = (sz.height() > 0) ? (int)sz.height() : 512;

	dom->setContainerSize(SkSize::Make(width, height));

	printf("mode: %s\n", use_gpu ? "gpu" : "cpu");
	printf("size: %d x %d\n", width, height);

	// -------------------------------------------------------------------------
	// GPU: GLX offscreen pbuffer + Ganesh context
	// -------------------------------------------------------------------------

	Display	*x_display = nullptr;
	GLXPbuffer pbuffer = 0;
	GLXContext gl_ctx	= nullptr;
	sk_sp<GrDirectContext> gr_ctx;

	if (use_gpu) {
		x_display = XOpenDisplay(nullptr);

		if (!x_display) {
			fprintf(stderr, "XOpenDisplay failed — is DISPLAY set?\n");
			return EXIT_FAILURE;
		}

		const int fb_attrs[] = {
			GLX_DRAWABLE_TYPE, GLX_PBUFFER_BIT,
			GLX_RENDER_TYPE, GLX_RGBA_BIT,
			GLX_RED_SIZE, 8,
			GLX_GREEN_SIZE, 8,
			GLX_BLUE_SIZE, 8,
			GLX_ALPHA_SIZE, 8,
			None
		};

		int n_configs = 0;
		GLXFBConfig *fb_configs = glXChooseFBConfig(
			x_display, DefaultScreen(x_display), fb_attrs, &n_configs);

		if (!fb_configs || n_configs < 1) {
			fprintf(stderr, "glXChooseFBConfig failed\n");
			return EXIT_FAILURE;
		}

		const int pb_attrs[] = {
			GLX_PBUFFER_WIDTH, width,
			GLX_PBUFFER_HEIGHT, height,
			None
		};

		pbuffer = glXCreatePbuffer(x_display, fb_configs[0], pb_attrs);
		gl_ctx = glXCreateNewContext(x_display, fb_configs[0], GLX_RGBA_TYPE, nullptr, True);

		XFree(fb_configs);

		if (!gl_ctx) {
			fprintf(stderr, "glXCreateNewContext failed\n");
			return EXIT_FAILURE;
		}

		if (!glXMakeContextCurrent(x_display, pbuffer, pbuffer, gl_ctx)) {
			fprintf(stderr, "glXMakeContextCurrent failed\n");
			return EXIT_FAILURE;
		}

		auto gl_interface = GrGLInterfaces::MakeGLX();

		if (!gl_interface) {
			fprintf(stderr, "GrGLInterfaces::MakeGLX failed\n");
			return EXIT_FAILURE;
		}

		gr_ctx = GrDirectContexts::MakeGL(gl_interface);

		if (!gr_ctx) {
			fprintf(stderr, "GrDirectContexts::MakeGL failed\n");
			return EXIT_FAILURE;
		}
	}

	// -------------------------------------------------------------------------
	// Surface
	// -------------------------------------------------------------------------

	sk_sp<SkSurface> surface = use_gpu
		? SkSurfaces::RenderTarget(gr_ctx.get(), skgpu::Budgeted::kNo, SkImageInfo::MakeN32Premul(width, height))
		: SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height))
	;

	if (!surface) {
		fprintf(stderr, "surface creation failed\n");
		return EXIT_FAILURE;
	}

	SkCanvas *canvas = surface->getCanvas();

	// -------------------------------------------------------------------------
	// Benchmark
	// -------------------------------------------------------------------------

	const int warmups = 5;
	const int iterations = 50;

	for (int i = 0; i < warmups; i++) {
		canvas->clear(SK_ColorTRANSPARENT);
		dom->render(canvas);
	}

	if (use_gpu) gr_ctx->flushAndSubmit(GrSyncCpu::kYes);

	double render_begin = now_ms();

	for (int i = 0; i < iterations; i++) {
		canvas->clear(SK_ColorTRANSPARENT);
		dom->render(canvas);
	}

	if (use_gpu) gr_ctx->flushAndSubmit(GrSyncCpu::kYes);

	double render_end = now_ms();

	printf("load:              %.3f ms\n", load_end - load_begin);
	printf("render total:      %.3f ms\n", render_end - render_begin);
	printf("render per frame:  %.3f ms\n", (render_end - render_begin) / iterations);

	// -------------------------------------------------------------------------
	// GPU cleanup
	// -------------------------------------------------------------------------

	if (use_gpu) {
		gr_ctx.reset();
		glXMakeContextCurrent(x_display, None, None, nullptr);
		glXDestroyContext(x_display, gl_ctx);
		glXDestroyPbuffer(x_display, pbuffer);
		XCloseDisplay(x_display);
	}

	return EXIT_SUCCESS;
}
