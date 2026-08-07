#include <cairo.h>
#include <librsvg/rsvg.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s file.svg\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *filename = argv[1];
	GError *error = NULL;

	double load_begin = now_ms();

	RsvgHandle *handle = rsvg_handle_new_from_file(filename, &error);

	double load_end = now_ms();

	if (!handle) {
		fprintf(stderr, "load failed: %s\n", error->message);
		g_error_free(error);
		return EXIT_FAILURE;
	}

	double width = 0.0;
	double height = 0.0;

	if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &width, &height)) {
		fprintf(stderr, "SVG has no intrinsic pixel dimensions\n");
		g_object_unref(handle);
		return EXIT_FAILURE;
	}

	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)width, (int)height);

	cairo_t *cr = cairo_create(surface);

	const RsvgRectangle viewport = {
		.x = 0.0,
		.y = 0.0,
		.width = width,
		.height = height,
	};

	const int warmups = 5;
	const int iterations = 50;

	for (int i = 0; i < warmups; ++i) {
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

		if (!rsvg_handle_render_document(handle, cr, &viewport, &error)) {
			fprintf(stderr, "render failed: %s\n", error->message);
			return EXIT_FAILURE;
		}
	}

	double render_begin = now_ms();

	for (int i = 0; i < iterations; ++i) {
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

		if (!rsvg_handle_render_document(handle, cr, &viewport, &error)) {
			fprintf(stderr, "render failed: %s\n", error->message);
			return EXIT_FAILURE;
		}
	}

	cairo_surface_flush(surface);

	double render_end = now_ms();

	printf("load:             %.3f ms\n", load_end - load_begin);
	printf("render total:     %.3f ms\n", render_end - render_begin);
	printf("render per frame: %.3f ms\n", (render_end - render_begin) / iterations);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	g_object_unref(handle);

	return EXIT_SUCCESS;
}
