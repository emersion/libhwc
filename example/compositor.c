#define _POSIX_C_SOURCE 200809L
#include <drm_fourcc.h>
#include <fcntl.h>
#include <libliftoff.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "common.h"

#define LAYERS_LEN 6

/* ARGB 8:8:8:8 */
static const uint32_t colors[] = {
	0xFFFF0000, /* red */
	0xFF00FF00, /* green */
	0xFF0000FF, /* blue */
	0xFFFFFF00, /* yellow */
};

static struct liftoff_layer *add_layer(int drm_fd, struct liftoff_output *output,
				       int x, int y, int width, int height,
				       bool with_alpha, struct dumb_fb *fb)
{
	static bool first = true;
	static size_t color_idx = 0;
	uint32_t color;
	struct liftoff_layer *layer;

	uint32_t format = with_alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
	if (!dumb_fb_init(fb, drm_fd, format, width, height)) {
		fprintf(stderr, "failed to create framebuffer\n");
		return NULL;
	}
	printf("Created FB %d with size %dx%d\n", fb->id, width, height);

	if (first) {
		color = 0xFFFFFFFF;
		first = false;
	} else {
		color = colors[color_idx];
		color_idx = (color_idx + 1) % (sizeof(colors) / sizeof(colors[0]));
	}

	dumb_fb_fill(fb, drm_fd, color);

	layer = liftoff_layer_create(output);
	liftoff_layer_set_property(layer, "FB_ID", fb->id);
	liftoff_layer_set_property(layer, "CRTC_X", x);
	liftoff_layer_set_property(layer, "CRTC_Y", y);
	liftoff_layer_set_property(layer, "CRTC_W", width);
	liftoff_layer_set_property(layer, "CRTC_H", height);
	liftoff_layer_set_property(layer, "SRC_X", 0);
	liftoff_layer_set_property(layer, "SRC_Y", 0);
	liftoff_layer_set_property(layer, "SRC_W", width << 16);
	liftoff_layer_set_property(layer, "SRC_H", height << 16);

	return layer;
}

/* Naive compositor for opaque buffers */
static void composite(int drm_fd, struct dumb_fb *dst_fb, struct dumb_fb *src_fb,
		      int dst_x, int dst_y)
{
	uint8_t *dst, *src;
	size_t y;

	dst = dumb_fb_map(dst_fb, drm_fd);
	src = dumb_fb_map(src_fb, drm_fd);

	for (y = 0; y < src_fb->height; y++) {
		memcpy(dst + dst_fb->stride * (dst_y + y) +
			     dst_x * sizeof(uint32_t),
		       src + src_fb->stride * y,
		       src_fb->width * sizeof(uint32_t));
	}

	munmap(dst, dst_fb->size);
	munmap(src, src_fb->size);
}

int main(int argc, char *argv[])
{
	int drm_fd;
	struct liftoff_display *display;
	drmModeRes *drm_res;
	drmModeCrtc *crtc;
	drmModeConnector *connector;
	struct liftoff_output *output;
	struct dumb_fb fbs[LAYERS_LEN] = {0};
	struct liftoff_layer *layers[LAYERS_LEN];
	drmModeAtomicReq *req;
	int ret;
	size_t i;

	drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		perror("open");
		return 1;
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
		perror("drmSetClientCap(UNIVERSAL_PLANES)");
		return 1;
	}
	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
		perror("drmSetClientCap(ATOMIC)");
		return 1;
	}

	display = liftoff_display_create(drm_fd);
	if (display == NULL) {
		perror("liftoff_display_create");
		return 1;
	}

	drm_res = drmModeGetResources(drm_fd);
	connector = pick_connector(drm_fd, drm_res);
	crtc = pick_crtc(drm_fd, drm_res, connector);
	disable_all_crtcs_except(drm_fd, drm_res, crtc->crtc_id);
	output = liftoff_output_create(display, crtc->crtc_id);
	drmModeFreeResources(drm_res);

	if (connector == NULL) {
		fprintf(stderr, "no connector found\n");
		return 1;
	}
	if (crtc == NULL || !crtc->mode_valid) {
		fprintf(stderr, "no CRTC found\n");
		return 1;
	}

	printf("Using connector %d, CRTC %d\n", connector->connector_id,
	       crtc->crtc_id);

	layers[0] = add_layer(drm_fd, output, 0, 0, crtc->mode.hdisplay,
			      crtc->mode.vdisplay, false, &fbs[0]);
	for (i = 1; i < LAYERS_LEN; i++) {
		layers[i] = add_layer(drm_fd, output, 100 * i, 100 * i,
				      256, 256, i % 2, &fbs[i]);
	}

	for (i = 0; i < LAYERS_LEN; i++) {
		liftoff_layer_set_property(layers[i], "zpos", i);
	}

	req = drmModeAtomicAlloc();
	if (!liftoff_display_apply(display, req)) {
		perror("liftoff_display_commit");
		return 1;
	}

	/* Composite layers that didn't make it into a plane */
	for (i = 1; i < LAYERS_LEN; i++) {
		if (liftoff_layer_get_plane_id(layers[i]) == 0) {
			composite(drm_fd, &fbs[0], &fbs[i], i * 100, i * 100);
		}
	}

	ret = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
	if (ret < 0) {
		perror("drmModeAtomicCommit");
		return false;
	}

	for (i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
		printf("Layer %zu got assigned to plane %u\n", i,
		       liftoff_layer_get_plane_id(layers[i]));
	}

	sleep(1);

	drmModeAtomicFree(req);
	for (i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
		liftoff_layer_destroy(layers[i]);
	}
	liftoff_output_destroy(output);
	drmModeFreeCrtc(crtc);
	drmModeFreeConnector(connector);
	liftoff_display_destroy(display);
	return 0;
}