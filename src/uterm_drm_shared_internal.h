/*
 * uterm - Linux User-Space Terminal
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

#ifndef UTERM_DRM_SHARED_INTERNAL_H
#define UTERM_DRM_SHARED_INTERNAL_H

#include <stdlib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "eloop.h"
#include "shl_timer.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

/* drm object */

struct drm_object {
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	uint32_t id;
};

/* drm display */

struct uterm_drm_display {
	struct drm_object connector;
	struct drm_object crtc;
	struct drm_object plane;

	drmModeModeInfo mode;
	uint32_t mode_blob_id;
	uint32_t crtc_index;
	uint32_t damage_blob_id;
	bool need_redraw;

	drmModeModeInfoPtr current_mode;
	drmModeModeInfo default_mode;
	drmModeModeInfo desired_mode;
	drmModeModeInfo original_mode;

	/* For legacy modesetting */
	uint32_t fb_id;

	int (*prepare_modeset)(struct uterm_display *disp, drmModeAtomicReqPtr rec);
	void (*done_modeset)(struct uterm_display *disp, int status);
};

int uterm_drm_display_set_dpms(struct uterm_display *disp, int state);
int uterm_drm_display_wait_pflip(struct uterm_display *disp);
int uterm_drm_prepare_commit(int fd, struct uterm_drm_display *ddrm, drmModeAtomicReq *req,
			     uint32_t fb, uint32_t width, uint32_t height);
int uterm_drm_display_swap(struct uterm_display *disp, uint32_t fb);
bool uterm_drm_is_swapping(struct uterm_display *disp);
bool uterm_drm_display_need_redraw(struct uterm_display *disp);
void uterm_drm_display_free_properties(struct uterm_display *disp);
void uterm_drm_display_set_damage(struct uterm_display *disp, size_t n_rect,
				  struct uterm_video_rect *damages);
bool uterm_drm_display_has_damage(struct uterm_display *disp);

/* drm video */

typedef void (*uterm_drm_page_flip_t)(struct uterm_display *disp);

struct uterm_drm_video {
	char *name;
	int fd;
	struct ev_fd *efd;
	uterm_drm_page_flip_t page_flip;
	void *data;
	struct shl_timer *timer;
	struct ev_timer *vt_timer;
	bool legacy;
	const struct display_ops *display_ops;
};

int uterm_drm_video_init(struct uterm_video *video, const char *node,
			 const struct display_ops *display_ops, uterm_drm_page_flip_t pflip,
			 void *data);
void uterm_drm_video_destroy(struct uterm_video *video);
int uterm_drm_video_hotplug(struct uterm_video *video, bool read_dpms, bool modeset);
int uterm_drm_video_wake_up(struct uterm_video *video);
void uterm_drm_video_sleep(struct uterm_video *video);
int uterm_drm_video_poll(struct uterm_video *video);
int uterm_drm_video_wait_pflip(struct uterm_video *video, unsigned int *mtimeout);
void uterm_drm_video_arm_vt_timer(struct uterm_video *video);

static inline void *uterm_drm_video_get_data(struct uterm_video *video)
{
	struct uterm_drm_video *v = video->data;

	return v->data;
}

#endif /* UTERM_DRM_SHARED_INTERNAL_H */
