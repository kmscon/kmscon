/*
 * video - Linux User-Space Terminal Video Handling
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
 * Video Control
 * Linux provides 2 famous ways to access the video hardware: FBDEV and DRM.
 * fbdev is the older one of both and is simply a mmap() of the framebuffer into
 * main memory. It does not allow 3D acceleration and if you need 2D
 * acceleration you should use libraries like cairo to draw into the framebuffer
 * provided by this library.
 * DRM is the new approach which provides 3D acceleration with mesa. It allows
 * much more configuration as fbdev and is the recommended way to access video
 * hardware on modern computers.
 * Modern mesa provides 3D acceleration on fbdev, too. This is used in systems
 * like Android. This will allow us to provide an fbdev backend here.
 *
 * Famous linux graphics systems like X.Org/X11 or Wayland use fbdev or DRM
 * internally to access the video hardware. This API allows low-level access to
 * fbdev and DRM without the need of X.Org/X11 or Wayland. If VT support is
 * enabled in your kernel, each application can run on a different VT. For
 * instance, X.Org may run on VT-7, Wayland on VT-8, your application on VT-9
 * and default consoles on VT-1 to VT-6. You can switch between them with
 * ctrl-alt-F1-F12.
 * If VT support is not available you need other ways to switch between
 * applications. See uterm_vt for more.
 */

#ifndef VIDEO_H
#define VIDEO_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include "shl/eloop.h"

struct display;
struct video;
struct video_module;

enum display_state {
	DISPLAY_ACTIVE,
	DISPLAY_ASLEEP,
	DISPLAY_INACTIVE,
	DISPLAY_GONE,
};

enum display_dpms {
	DPMS_ON,
	DPMS_STANDBY,
	DPMS_SUSPEND,
	DPMS_OFF,
	DPMS_UNKNOWN,
};

enum video_action {
	VIDEO_WAKE_UP,
	VIDEO_SLEEP,
	VIDEO_NEW,
	VIDEO_GONE,
	VIDEO_REFRESH,
};

struct video_hotplug {
	struct display *display;
	int action;
};

enum display_action {
	DISPLAY_PAGE_FLIP,
};

struct display_event {
	int action;
};

struct video_buffer {
	unsigned int width;
	unsigned int height;
	unsigned int stride;
	uint8_t data[];
};

struct video_blend_req {
	const struct video_buffer *buf;
	unsigned int x;
	unsigned int y;
	uint8_t fr;
	uint8_t fg;
	uint8_t fb;
	uint8_t br;
	uint8_t bg;
	uint8_t bb;
};

/*
 * This matches struct drm_mode_rect, for damage tracking
 */
struct video_rect {
	int32_t x1;
	int32_t y1;
	int32_t x2;
	int32_t y2;
};

typedef void (*video_cb)(struct video *video, struct video_hotplug *arg, void *data);
typedef void (*display_cb)(struct display *disp, struct display_event *arg, void *data);

/* misc */

const char *dpms_to_name(enum display_dpms dpms);

/* display interface */

void display_ref(struct display *disp);
void display_unref(struct display *disp);
bool display_is_drm(struct display *disp);
bool display_has_opengl(struct display *disp);
bool display_supports_damage(struct display *disp);
const char *display_backend_name(struct display *disp);
const char *display_name(struct display *disp);
struct display *display_next(struct display *disp);

int display_register_cb(struct display *disp, display_cb cb, void *data);
void display_unregister_cb(struct display *disp, display_cb cb, void *data);

unsigned int display_get_width(struct display *disp);
unsigned int display_get_height(struct display *disp);
int display_get_state(struct display *disp);
int display_set_dpms(struct display *disp, enum display_dpms state);
enum display_dpms display_get_dpms(const struct display *disp);

int display_use(struct display *disp);
int display_swap(struct display *disp);
bool display_is_swapping(struct display *disp);
void display_set_need_redraw(struct display *disp);
bool display_need_redraw(struct display *disp);

int display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b);

/* cursor interface */
#define VIDEO_CURSOR_MAX_SIZE 64
int display_setup_cursor(struct display *disp, const uint32_t *pixels, unsigned int width,
			 unsigned int height, int hot_x, int hot_y);
void display_destroy_cursor(struct display *disp);
int display_show_cursor(struct display *disp, int32_t x, int32_t y);
int display_hide_cursor(struct display *disp);
void display_set_cursor_offset(struct display *disp, int32_t x, int32_t y);

int display_blendv(struct display *disp, const struct video_blend_req *req, size_t num);
void display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages);
bool display_has_damage(struct display *disp);

/* video interface */

int video_new(struct video **out, struct ev_eloop *eloop, int fd, const char *backend,
	      unsigned int desired_width, unsigned int desired_height, bool use_original);
void video_ref(struct video *video);
void video_unref(struct video *video);

struct display *video_get_displays(struct video *video);
int video_register_cb(struct video *video, video_cb cb, void *data);
void video_unregister_cb(struct video *video, video_cb cb, void *data);

int video_register(const struct video_module *ops);
void video_unregister(const char *name);
void video_sleep(struct video *video);
int video_wake_up(struct video *video);
bool video_is_awake(struct video *video);
void video_poll(struct video *video);

#ifdef BUILD_ENABLE_VIDEO_DRM2D
extern struct video_module drm2d_module;

static inline void video_register_drm2d(void)
{
	video_register(&drm2d_module);
}

#else
static inline void video_register_drm2d(void) {}
#endif

#ifdef BUILD_ENABLE_VIDEO_FBDEV
extern struct video_module fbdev_module;

static inline void video_register_fbdev(void)
{
	video_register(&fbdev_module);
}
#else
static inline void video_register_fbdev(void) {}
#endif

#endif /* VIDEO_H */
