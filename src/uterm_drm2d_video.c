/*
 * uterm - Linux User-Space Terminal drm2d module
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

/*
 * DRM Video backend using dumb buffer objects
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libdrm/drm_fourcc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "eloop.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "uterm_drm2d_internal.h"
#include "uterm_drm_shared_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "video_drm2d"

static int display_init(struct uterm_display *disp)
{
	struct uterm_drm2d_display *d2d;
	int ret;

	d2d = malloc(sizeof(*d2d));
	if (!d2d)
		return -ENOMEM;
	memset(d2d, 0, sizeof(*d2d));

	ret = uterm_drm_display_init(disp, d2d);
	if (ret) {
		free(d2d);
		return ret;
	}

	return 0;
}

static void display_destroy(struct uterm_display *disp)
{
	free(uterm_drm_display_get_data(disp));
	uterm_drm_display_destroy(disp);
}

static int drm_addfb2(int fd, uint32_t width, uint32_t height, struct uterm_drm2d_rb *rb)
{
	uint32_t handles[4] = {rb->handle, 0, 0, 0};
	uint32_t pitches[4] = {rb->stride, 0, 0, 0};
	uint32_t offsets[4] = {0, 0, 0, 0};

	return drmModeAddFB2(fd, width, height, DRM_FORMAT_XRGB8888, handles, pitches, offsets,
			     &rb->id, 0);
}

static int init_rb(struct uterm_display *disp, struct uterm_drm2d_rb *rb)
{
	int ret, r;
	struct uterm_video *video = disp->video;
	struct uterm_drm_video *vdrm = video->data;
	uint32_t width, height;
	uint64_t mmap_offset;

	width = disp->width;
	height = disp->height;

	if (drmModeCreateDumbBuffer(vdrm->fd, width, height, 32, 0, &rb->handle, &rb->stride,
				    &rb->size)) {
		log_err("cannot create dumb drm buffer");
		return -EFAULT;
	}

	ret = drm_addfb2(vdrm->fd, width, height, rb);
	if (ret) {
		log_err("cannot add drm-fb");
		ret = -EFAULT;
		goto err_buf;
	}

	ret = drmModeMapDumbBuffer(vdrm->fd, rb->handle, &mmap_offset);
	if (ret) {
		log_err("Cannot map dumb buffer");
		goto err_fb;
	}

	rb->map = mmap(0, rb->size, PROT_READ | PROT_WRITE, MAP_SHARED, vdrm->fd, mmap_offset);
	if (rb->map == MAP_FAILED) {
		log_err("cannot mmap dumb buffer");
		ret = -EFAULT;
		goto err_fb;
	}
	memset(rb->map, 0, rb->size);

	return 0;

err_fb:
	drmModeRmFB(vdrm->fd, rb->id);
err_buf:
	r = drmModeDestroyDumbBuffer(vdrm->fd, rb->handle);
	if (r)
		log_warning("cannot destroy dumb buffer (%d/%d): %m", ret, errno);

	return ret;
}

static void destroy_rb(struct uterm_display *disp, struct uterm_drm2d_rb *rb)
{
	struct uterm_drm_video *vdrm = disp->video->data;
	int ret;

	munmap(rb->map, rb->size);
	drmModeRmFB(vdrm->fd, rb->id);
	ret = drmModeDestroyDumbBuffer(vdrm->fd, rb->handle);
	if (ret)
		log_warning("cannot destroy dumb buffer (%d/%d): %m", ret, errno);
}

static int display_activate(struct uterm_display *disp)
{
	struct uterm_video *video = disp->video;
	struct uterm_drm_video *vdrm = video->data;
	struct uterm_drm_display *ddrm = disp->data;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);
	int ret;
	drmModeModeInfo *minfo;

	if (video->use_original)
		ddrm->current_mode = ddrm->original_mode;
	else
		ddrm->current_mode = ddrm->desired_mode;
	minfo = &ddrm->current_mode->info;
	disp->width = minfo->hdisplay;
	disp->height = minfo->vdisplay;

	log_info("activating display %p to %ux%u", disp, minfo->hdisplay, minfo->vdisplay);

	ret = uterm_drm_display_activate(disp, vdrm->fd);
	if (ret)
		return ret;

	d2d->current_rb = 0;

	ret = init_rb(disp, &d2d->rb[0]);
	if (ret)
		goto err_saved;

	ret = init_rb(disp, &d2d->rb[1]);
	if (ret)
		goto err_rb;

	ret = drmModeSetCrtc(vdrm->fd, ddrm->crtc_id, d2d->rb[0].id, 0, 0, &ddrm->conn_id, 1,
			     minfo);

	if (ret && ddrm->current_mode == ddrm->desired_mode &&
	    ddrm->current_mode != ddrm->default_mode) {
		ddrm->current_mode = ddrm->desired_mode = ddrm->default_mode;
		ret = -EAGAIN;
		goto err_fb;
	} else if (ret) {
		log_err("cannot set drm-crtc");
		ret = -EFAULT;
		goto err_fb;
	}

	disp->flags |= DISPLAY_ONLINE;
	return 0;

err_fb:
	destroy_rb(disp, &d2d->rb[1]);
err_rb:
	destroy_rb(disp, &d2d->rb[0]);
err_saved:
	disp->width = 0;
	disp->height = 0;
	uterm_drm_display_deactivate(disp, vdrm->fd);
	return ret;
}

static void display_deactivate(struct uterm_display *disp)
{
	struct uterm_drm_video *vdrm;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);

	vdrm = disp->video->data;
	log_info("deactivating display %p", disp);

	uterm_drm_display_deactivate(disp, vdrm->fd);

	destroy_rb(disp, &d2d->rb[1]);
	destroy_rb(disp, &d2d->rb[0]);
	disp->width = 0;
	disp->height = 0;
}

static int display_swap(struct uterm_display *disp, bool immediate)
{
	int ret, rb;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);

	rb = d2d->current_rb ^ 1;
	if (immediate)
		ret = uterm_drm_modeset(disp, d2d->rb[rb].id);
	else
		ret = uterm_drm_display_swap(disp, d2d->rb[rb].id);
	if (ret)
		return ret;

	d2d->current_rb = rb;
	return 0;
}

static const struct display_ops drm2d_display_ops = {
	.init = display_init,
	.destroy = display_destroy,
	.activate = display_activate,
	.deactivate = display_deactivate,
	.set_dpms = uterm_drm_display_set_dpms,
	.use = NULL,
	.swap = display_swap,
	.is_swapping = uterm_drm_is_swapping,
	.fake_blendv = uterm_drm2d_display_fake_blendv,
	.fill = uterm_drm2d_display_fill,
};

static void show_displays(struct uterm_video *video)
{
	struct uterm_display *iter;
	struct uterm_drm2d_display *d2d;
	struct uterm_drm2d_rb *rb;
	struct shl_dlist *i;

	if (!video_is_awake(video))
		return;

	shl_dlist_for_each(i, &video->displays)
	{
		iter = shl_dlist_entry(i, struct uterm_display, list);

		if (!display_is_online(iter))
			continue;
		if (iter->dpms != UTERM_DPMS_ON)
			continue;

		/* We use double-buffering so there might be no free back-buffer
		 * here. Hence, draw into the current (pending) front-buffer and
		 * wait for possible page-flips to complete. This might cause
		 * tearing but that's acceptable as this is only called during
		 * wakeup/sleep. */

		d2d = uterm_drm_display_get_data(iter);
		rb = &d2d->rb[d2d->current_rb];
		memset(rb->map, 0, rb->size);
		uterm_drm_display_wait_pflip(iter);
	}
}

static int video_init(struct uterm_video *video, const char *node)
{
	int ret;
	uint64_t has_dumb;
	struct uterm_drm_video *vdrm;

	ret = uterm_drm_video_init(video, node, &drm2d_display_ops, NULL, NULL);
	if (ret)
		return ret;
	vdrm = video->data;

	log_debug("initialize 2D layer on %p", video);

	if (drmGetCap(vdrm->fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 || !has_dumb) {
		log_err("driver does not support dumb buffers");
		uterm_drm_video_destroy(video);
		return -EOPNOTSUPP;
	}

	return 0;
}

static void video_destroy(struct uterm_video *video)
{
	log_info("free drm video device %p", video);
	uterm_drm_video_destroy(video);
}

static int video_poll(struct uterm_video *video)
{
	return uterm_drm_video_poll(video);
}

static void video_sleep(struct uterm_video *video)
{
	show_displays(video);
	uterm_drm_video_sleep(video);
}

static int video_wake_up(struct uterm_video *video)
{
	int ret;

	ret = uterm_drm_video_wake_up(video);
	if (ret) {
		uterm_drm_video_arm_vt_timer(video);
		return ret;
	}

	show_displays(video);
	return 0;
}

struct uterm_video_module drm2d_module = {
	.name = "drm2d",
	.owner = NULL,
	.ops =
		{
			.init = video_init,
			.destroy = video_destroy,
			.poll = video_poll,
			.sleep = video_sleep,
			.wake_up = video_wake_up,
		},
};
