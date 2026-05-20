/*
 * Kmscon - DRM Shared Internal
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Internal definitions */

#ifndef DRM_SHARED_INTERNAL_H
#define DRM_SHARED_INTERNAL_H

#include <stdlib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "shl/eloop.h"
#include "shl/timer.h"
#include "video.h"
#include "video_internal.h"

/* drm object */

struct drm_object {
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	uint32_t id;
};

/* drm display */

struct drm_cursor {
	uint32_t bo_handle;
	uint32_t fb_id;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint8_t *map;
	uint64_t map_size;
	bool active;
	bool visible;
	int32_t x;
	int32_t y;
	int32_t hot_x;
	int32_t hot_y;
	int32_t off_x;
	int32_t off_y;
};

struct drm_display {
	struct drm_object connector;
	struct drm_object crtc;
	struct drm_object plane;
	struct drm_object cursor_plane;

	drmModeModeInfo mode;
	uint32_t mode_blob_id;
	uint32_t crtc_index;
	uint32_t damage_blob_id;

	drmModeModeInfoPtr current_mode;
	drmModeModeInfo default_mode;
	drmModeModeInfo desired_mode;
	drmModeModeInfo original_mode;

	/* For legacy modesetting */
	uint32_t fb_id;

	struct drm_cursor cursor;

	int (*prepare_modeset)(struct display *disp, drmModeAtomicReqPtr rec);
	void (*done_modeset)(struct display *disp, int status);
};

int drm_display_set_dpms(struct display *disp, enum display_dpms dpms);

int drm_display_setup_cursor(struct display *disp, const uint32_t *pixels, unsigned int width,
			     unsigned int height, int hot_x, int hot_y);
void drm_display_destroy_cursor(struct display *disp);
int drm_display_show_cursor(struct display *disp, int32_t x, int32_t y);
int drm_display_hide_cursor(struct display *disp);
void drm_display_set_cursor_offset(struct display *disp, int32_t x, int32_t y);
int drm_display_wait_pflip(struct display *disp);
int drm_prepare_commit(int fd, struct drm_display *ddrm, drmModeAtomicReq *req, uint32_t fb,
		       uint32_t width, uint32_t height, bool cursor_hotspot);
int drm_display_swap(struct display *disp, uint32_t fb);
bool drm_is_swapping(struct display *disp);
void drm_display_free_properties(struct display *disp);
void drm_display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages);
bool drm_display_has_damage(struct display *disp);

/* drm video */

typedef void (*drm_page_flip_t)(struct display *disp);

struct drm_video {
	char *name;
	int fd;
	struct ev_fd *efd;
	drm_page_flip_t page_flip;
	void *data;
	struct shl_timer *timer;
	struct ev_timer *vt_timer;
	bool legacy;
	bool master;
	bool cursor_hotspot;
	const struct display_ops *display_ops;
};

int drm_video_init(struct video *video, int fd, const struct display_ops *display_ops,
		   drm_page_flip_t pflip, void *data);
void drm_video_destroy(struct video *video);
int drm_video_hotplug(struct video *video, bool read_dpms, bool modeset);
int drm_video_wake_up(struct video *video);
void drm_video_sleep(struct video *video);
int drm_video_poll(struct video *video);
int drm_video_wait_pflip(struct video *video, unsigned int *mtimeout);
void drm_video_arm_vt_timer(struct video *video);

static inline void *drm_video_get_data(struct video *video)
{
	struct drm_video *v = video->data;

	return v->data;
}

#endif /* DRM_SHARED_INTERNAL_H */
