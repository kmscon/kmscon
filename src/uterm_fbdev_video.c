/*
 * uterm - Linux User-Space Terminal fbdev module
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
 * FBDEV Video backend
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "shl_log.h"
#include "shl_misc.h"
#include "uterm_fbdev_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "video_fbdev"

static int display_schedule_vblank_timer(struct fbdev_display *fbdev)
{
	int ret;

	if (fbdev->vblank_scheduled)
		return 0;

	ret = ev_timer_update(fbdev->vblank_timer, &fbdev->vblank_spec);
	if (ret)
		return ret;

	fbdev->vblank_scheduled = true;
	return 0;
}

static void display_set_vblank_timer(struct fbdev_display *fbdev, unsigned int msecs)
{
	if (msecs >= 1000)
		msecs = 999;
	else if (msecs == 0)
		msecs = 15;

	fbdev->vblank_spec.it_value.tv_nsec = msecs * 1000 * 1000;
}

static void display_vblank_timer_event(struct ev_timer *timer, uint64_t expirations, void *data)
{
	struct uterm_display *disp = data;
	struct fbdev_display *fbdev = disp->data;

	fbdev->vblank_scheduled = false;
	DISPLAY_CB(disp, UTERM_PAGE_FLIP);
}

static int display_init(struct uterm_display *disp)
{
	struct fbdev_display *fbdev;
	int ret;

	fbdev = malloc(sizeof(*fbdev));
	if (!fbdev)
		return -ENOMEM;
	memset(fbdev, 0, sizeof(*fbdev));
	disp->data = fbdev;
	disp->dpms = UTERM_DPMS_UNKNOWN;

	fbdev->vblank_spec.it_value.tv_nsec = 15 * 1000 * 1000;
	ret = ev_timer_new(&fbdev->vblank_timer, NULL, display_vblank_timer_event, disp, NULL,
			   NULL);
	if (ret)
		goto err_free;

	return 0;

err_free:
	free(fbdev);
	return ret;
}

static void display_destroy(struct uterm_display *disp)
{
	struct fbdev_display *fbdev = disp->data;
	ev_eloop_rm_timer(fbdev->vblank_timer);
	ev_timer_unref(fbdev->vblank_timer);
	free(disp->data);
}

static int refresh_info(struct uterm_display *disp)
{
	int ret;
	struct fbdev_display *dfb = disp->data;

	ret = ioctl(dfb->fd, FBIOGET_FSCREENINFO, &dfb->finfo);
	if (ret) {
		log_err("cannot get finfo (%d): %m", errno);
		return -EFAULT;
	}

	ret = ioctl(dfb->fd, FBIOGET_VSCREENINFO, &dfb->vinfo);
	if (ret) {
		log_err("cannot get vinfo (%d): %m", errno);
		return -EFAULT;
	}

	return 0;
}

static int display_activate_force(struct uterm_display *disp, bool force)
{
	static const char depths[] = {32, 24, 16, 0};
	struct fbdev_display *dfb = disp->data;
	struct fb_var_screeninfo *vinfo;
	struct fb_fix_screeninfo *finfo;
	int ret, i;
	uint64_t quot;
	size_t len;
	unsigned int val;

	if (!force && (disp->flags & DISPLAY_ONLINE))
		return 0;

	dfb->fd = open(dfb->node, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (dfb->fd < 0) {
		log_err("cannot open %s (%d): %m", dfb->node, errno);
		return -EFAULT;
	}

	ret = refresh_info(disp);
	if (ret)
		goto err_close;

	finfo = &dfb->finfo;
	vinfo = &dfb->vinfo;

	vinfo->xoffset = 0;
	vinfo->yoffset = 0;
	vinfo->activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
	vinfo->xres_virtual = vinfo->xres;
	vinfo->yres_virtual = vinfo->yres * 2;
	disp->flags |= DISPLAY_DBUF;

	/* udlfb is broken as it reports the sizes of the virtual framebuffer
	 * (even mmap() accepts it) but the actual size that we can access
	 * without segfaults is the _real_ framebuffer. Therefore, disable
	 * double-buffering for it.
	 * TODO: fix this kernel-side!
	 * TODO: There are so many broken fbdev drivers that just accept any
	 * virtual FB sizes and then break mmap that we now disable
	 * double-buffering entirely. We might instead add a white-list or
	 * optional command-line argument to re-enable it. */
	if (true || !strcmp(finfo->id, "udlfb")) {
		disp->flags &= ~DISPLAY_DBUF;
		vinfo->yres_virtual = vinfo->yres;
	}

	ret = ioctl(dfb->fd, FBIOPUT_VSCREENINFO, vinfo);
	if (ret) {
		disp->flags &= ~DISPLAY_DBUF;
		vinfo->yres_virtual = vinfo->yres;
		ret = ioctl(dfb->fd, FBIOPUT_VSCREENINFO, vinfo);
		if (ret) {
			log_debug("cannot reset fb offsets (%d): %m", errno);
			goto err_close;
		}
	}

	if (disp->flags & DISPLAY_DBUF)
		log_debug("enable double buffering");
	else
		log_debug("disable double buffering");

	ret = refresh_info(disp);
	if (ret)
		goto err_close;

	/* We require TRUECOLOR mode here. That is, each pixel has a color value
	 * that is split into rgba values that we can set directly. Other visual
	 * modes like pseudocolor or direct-color do not provide this. As I have
	 * never seen a device that does not support TRUECOLOR, I think we can
	 * ignore them here. */
	if (finfo->visual != FB_VISUAL_TRUECOLOR || vinfo->bits_per_pixel != 32) {
		for (i = 0; depths[i]; ++i) {
			/* Try to set a new mode and if it's successful... */
			struct fb_var_screeninfo vinfo_new = *vinfo;
			vinfo_new.bits_per_pixel = depths[i];
			vinfo_new.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;

			ret = ioctl(dfb->fd, FBIOPUT_VSCREENINFO, &vinfo_new);
			if (ret < 0)
				continue;

			/* ... keep it. */
			*vinfo = vinfo_new;

			ret = refresh_info(disp);
			if (ret)
				goto err_close;

			if (finfo->visual == FB_VISUAL_TRUECOLOR)
				break;
		}
	}

	if (vinfo->bits_per_pixel != 32 && vinfo->bits_per_pixel != 24 &&
	    vinfo->bits_per_pixel != 16) {
		log_error("device %s does not support 16/32 bpp but: %u", dfb->node,
			  vinfo->bits_per_pixel);
		ret = -EFAULT;
		goto err_close;
	}

	if (vinfo->xres_virtual < vinfo->xres ||
	    (disp->flags & DISPLAY_DBUF && vinfo->yres_virtual < vinfo->yres * 2) ||
	    vinfo->yres_virtual < vinfo->yres) {
		log_warning("device %s has weird virtual buffer sizes (%d %d %d %d)", dfb->node,
			    vinfo->xres, vinfo->xres_virtual, vinfo->yres, vinfo->yres_virtual);
	}

	if (finfo->visual != FB_VISUAL_TRUECOLOR) {
		log_error("device %s does not support true-color", dfb->node);
		ret = -EFAULT;
		goto err_close;
	}

	if (vinfo->red.length > 8 || vinfo->green.length > 8 || vinfo->blue.length > 8) {
		log_error("device %s uses unusual color-ranges", dfb->node);
		ret = -EFAULT;
		goto err_close;
	}

	log_info("activating display %s to %ux%u %u bpp", dfb->node, vinfo->xres, vinfo->yres,
		 vinfo->bits_per_pixel);

	/* calculate monitor rate, default is 60 Hz */
	quot = (vinfo->upper_margin + vinfo->lower_margin + vinfo->yres);
	quot *= (vinfo->left_margin + vinfo->right_margin + vinfo->xres);
	quot *= vinfo->pixclock;
	if (quot) {
		dfb->rate = 1000000000000000LLU / quot;
	} else {
		dfb->rate = 60 * 1000;
		log_warning("cannot read monitor refresh rate, forcing 60 Hz");
	}

	if (dfb->rate == 0) {
		log_warning("monitor refresh rate is 0 Hz, forcing it to 1 Hz");
		dfb->rate = 1;
	} else if (dfb->rate > 200000) {
		log_warning("monitor refresh rate is >200 Hz (%u Hz), forcing it to 200 Hz",
			    dfb->rate / 1000);
		dfb->rate = 200000;
	}

	val = 1000000 / dfb->rate;
	display_set_vblank_timer(dfb, val);
	log_debug("vblank timer: %u ms, monitor refresh rate: %u Hz", val, dfb->rate / 1000);

	len = finfo->line_length * vinfo->yres;
	if (disp->flags & DISPLAY_DBUF)
		len *= 2;

	dfb->map = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, dfb->fd, 0);
	if (dfb->map == MAP_FAILED) {
		log_error("cannot mmap device %s (%d): %m", dfb->node, errno);
		ret = -EFAULT;
		goto err_close;
	}

	memset(dfb->map, 0, len);
	dfb->xres = vinfo->xres;
	dfb->yres = vinfo->yres;
	dfb->len = len;
	dfb->stride = finfo->line_length;
	dfb->bufid = 0;
	dfb->Bpp = vinfo->bits_per_pixel / 8;
	dfb->off_r = vinfo->red.offset;
	dfb->len_r = vinfo->red.length;
	dfb->off_g = vinfo->green.offset;
	dfb->len_g = vinfo->green.length;
	dfb->off_b = vinfo->blue.offset;
	dfb->len_b = vinfo->blue.length;
	dfb->dither_r = 0;
	dfb->dither_g = 0;
	dfb->dither_b = 0;
	dfb->xrgb32 = false;
	dfb->rgb16 = false;
	if (dfb->len_r == 8 && dfb->len_g == 8 && dfb->len_b == 8 && dfb->off_r == 16 &&
	    dfb->off_g == 8 && dfb->off_b == 0 && dfb->Bpp == 4)
		dfb->xrgb32 = true;
	else if (dfb->len_r == 5 && dfb->len_g == 6 && dfb->len_b == 5 && dfb->off_r == 11 &&
		 dfb->off_g == 5 && dfb->off_b == 0 && dfb->Bpp == 2)
		dfb->rgb16 = true;
	else if (dfb->len_r == 8 && dfb->len_g == 8 && dfb->len_b == 8 && dfb->off_r == 16 &&
		 dfb->off_g == 8 && dfb->off_b == 0 && dfb->Bpp == 3)
		dfb->rgb24 = true;

	/* TODO: make dithering configurable */
	disp->flags |= DISPLAY_DITHERING;
	disp->width = dfb->xres;
	disp->height = dfb->yres;

	disp->flags |= DISPLAY_ONLINE;
	return 0;

err_close:
	close(dfb->fd);
	return ret;
}

static void display_deactivate_force(struct uterm_display *disp, bool force)
{
	struct fbdev_display *dfb = disp->data;

	log_info("deactivating device %s", dfb->node);

	if (dfb->map) {
		memset(dfb->map, 0, dfb->len);
		munmap(dfb->map, dfb->len);
		close(dfb->fd);
		dfb->map = NULL;
	}
	if (!force) {
		disp->width = 0;
		disp->height = 0;
		disp->flags &= ~DISPLAY_ONLINE;
	}
}

static int display_set_dpms(struct uterm_display *disp, int state)
{
	int set, ret;
	struct fbdev_display *dfb = disp->data;

	switch (state) {
	case UTERM_DPMS_ON:
		set = FB_BLANK_UNBLANK;
		break;
	case UTERM_DPMS_STANDBY:
		set = FB_BLANK_NORMAL;
		break;
	case UTERM_DPMS_SUSPEND:
		set = FB_BLANK_NORMAL;
		break;
	case UTERM_DPMS_OFF:
		set = FB_BLANK_POWERDOWN;
		break;
	default:
		return -EINVAL;
	}

	log_info("setting DPMS of device %p to %s", dfb->node, uterm_dpms_to_name(state));

	ret = ioctl(dfb->fd, FBIOBLANK, set);
	if (ret) {
		log_error("cannot set DPMS on %s (%d): %m", dfb->node, errno);
		return -EFAULT;
	}

	disp->dpms = state;
	return 0;
}

static int display_swap(struct uterm_display *disp)
{
	struct fbdev_display *dfb = disp->data;
	struct fb_var_screeninfo *vinfo;
	int ret;

	if (!(disp->flags & DISPLAY_DBUF))
		return display_schedule_vblank_timer(dfb);

	vinfo = &dfb->vinfo;
	vinfo->activate = FB_ACTIVATE_VBL;

	if (!dfb->bufid)
		vinfo->yoffset = dfb->yres;
	else
		vinfo->yoffset = 0;

	ret = ioctl(dfb->fd, FBIOPUT_VSCREENINFO, vinfo);
	if (ret) {
		log_warning("cannot swap buffers on %s (%d): %m", dfb->node, errno);
		return -EFAULT;
	}

	dfb->bufid ^= 1;
	return display_schedule_vblank_timer(dfb);
}

static bool display_is_swapping(struct uterm_display *disp)
{
	struct fbdev_display *fbdev = disp->data;

	return fbdev->vblank_scheduled;
}

static const struct display_ops fbdev_display_ops = {
	.init = display_init,
	.destroy = display_destroy,
	.set_dpms = display_set_dpms,
	.use = NULL,
	.swap = display_swap,
	.is_swapping = display_is_swapping,
	.need_redraw = NULL,
	.fake_blendv = uterm_fbdev_display_fake_blendv,
	.fill = uterm_fbdev_display_fill,
	.set_damage = NULL,
};

static void intro_idle_event(struct ev_eloop *eloop, void *unused, void *data)
{
	struct uterm_video *video = data;
	struct fbdev_video *vfb = video->data;
	struct uterm_display *disp;
	struct fbdev_display *dfb;
	int ret;

	vfb->pending_intro = false;
	ev_eloop_unregister_idle_cb(eloop, intro_idle_event, data, EV_NORMAL);

	ret = display_new(&disp, &fbdev_display_ops, video, "fbdev");
	if (ret) {
		log_error("cannot create fbdev display: %d", ret);
		return;
	}

	dfb = disp->data;
	dfb->node = vfb->node;

	ret = ev_eloop_add_timer(video->eloop, dfb->vblank_timer);
	if (ret) {
		log_error("cannot add fbdev timer: %d", ret);
		return;
	}

	ret = uterm_display_bind(disp);
	if (ret) {
		log_error("cannot bind fbdev display: %d", ret);
		uterm_display_unref(disp);
		ev_eloop_rm_timer(dfb->vblank_timer);
		return;
	}
	uterm_display_ready(disp);
	uterm_display_unref(disp);
}

static int video_init(struct uterm_video *video, const char *node)
{
	int ret;
	struct fbdev_video *vfb;

	log_info("new device on %s", node);

	vfb = malloc(sizeof(*vfb));
	if (!vfb)
		return -ENOMEM;
	memset(vfb, 0, sizeof(*vfb));
	video->data = vfb;

	vfb->node = strdup(node);
	if (!vfb->node) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = ev_eloop_register_idle_cb(video->eloop, intro_idle_event, video, EV_NORMAL);
	if (ret) {
		log_error("cannot register idle event: %d", ret);
		goto err_node;
	}
	vfb->pending_intro = true;

	return 0;

err_node:
	free(vfb->node);
err_free:
	free(vfb);
	return ret;
}

static void video_destroy(struct uterm_video *video)
{
	struct fbdev_video *vfb = video->data;

	log_info("free device on %s", vfb->node);

	if (vfb->pending_intro)
		ev_eloop_unregister_idle_cb(video->eloop, intro_idle_event, video, EV_NORMAL);

	free(vfb->node);
	free(vfb);
}

static void video_sleep(struct uterm_video *video)
{
	struct uterm_display *iter;
	struct shl_dlist *i;

	shl_dlist_for_each(i, &video->displays)
	{
		iter = shl_dlist_entry(i, struct uterm_display, list);

		if (!display_is_online(iter))
			continue;

		display_deactivate_force(iter, true);
	}
}

static int video_wake_up(struct uterm_video *video)
{
	struct uterm_display *iter;
	struct shl_dlist *i;
	int ret;

	video->flags |= VIDEO_AWAKE;
	shl_dlist_for_each(i, &video->displays)
	{
		iter = shl_dlist_entry(i, struct uterm_display, list);

		if (!display_is_online(iter)) {
			ret = display_activate_force(iter, false);
			if (ret)
				return ret;
		}

		ret = display_activate_force(iter, true);
		if (ret)
			return ret;

		if (iter->dpms != UTERM_DPMS_UNKNOWN)
			display_set_dpms(iter, iter->dpms);
	}

	return 0;
}

struct uterm_video_module fbdev_module = {.name = "fbdev",
					  .owner = NULL,
					  .ops = {
						  .init = video_init,
						  .destroy = video_destroy,
						  .poll = NULL,
						  .sleep = video_sleep,
						  .wake_up = video_wake_up,
					  }};
