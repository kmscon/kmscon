/*
 * Kmscon - Video backend
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

#ifndef VIDEO_INTERNAL_H
#define VIDEO_INTERNAL_H

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include "shl/dlist.h"
#include "shl/eloop.h"
#include "shl/hook.h"
#include "video.h"

/* backend-operations */

struct display_ops {
	int (*init)(struct display *display);
	void (*destroy)(struct display *display);
	int (*set_dpms)(struct display *disp, enum display_dpms dpms);
	int (*use)(struct display *disp);
	int (*swap)(struct display *disp);
	bool (*is_swapping)(struct display *disp);
	int (*blendv)(struct display *disp, const struct video_blend_req *req, size_t num);
	int (*clear)(struct display *disp, uint8_t r, uint8_t g, uint8_t b);
	void (*set_damage)(struct display *disp, size_t n_rect, struct video_rect *damages);
	bool (*has_damage)(struct display *disp);
	int (*setup_cursor)(struct display *disp, const uint32_t *pixels, unsigned int width,
			    unsigned int height, int hot_x, int hot_y);
	void (*destroy_cursor)(struct display *disp);
	int (*show_cursor)(struct display *disp, int32_t x, int32_t y);
	int (*hide_cursor)(struct display *disp);
	void (*set_cursor_offset)(struct display *disp, int32_t x, int32_t y);
};

struct video_ops {
	int (*init)(struct video *video, int fd);
	void (*destroy)(struct video *video);
	int (*poll)(struct video *video);
	void (*sleep)(struct video *video);
	int (*wake_up)(struct video *video);
};

struct video_module {
	const char *name;
	struct shl_module *owner;
	const struct video_ops ops;
};

#define VIDEO_CALL(func, els, ...) (func ? func(__VA_ARGS__) : els)

/* display */

#define DISPLAY_ONLINE 0x01
#define DISPLAY_VSYNC 0x02
#define DISPLAY_AVAILABLE 0x04
#define DISPLAY_OPEN 0x08
#define DISPLAY_DBUF 0x10
#define DISPLAY_DITHERING 0x20
#define DISPLAY_PFLIP 0x40
#define DISPLAY_OPENGL 0x80
#define DISPLAY_INUSE 0x100
#define DISPLAY_DAMAGE 0x200
#define DISPLAY_NEED_REDRAW 0x400

struct display {
	char *name;
	struct shl_dlist list;
	unsigned long ref;
	unsigned int flags;
	unsigned int width;
	unsigned int height;

	struct video *video;

	struct shl_hook *hook;
	enum display_dpms dpms;

	const struct display_ops *ops;
	void *data;
};

int display_new(struct display **out, const struct display_ops *ops, struct video *video,
		const char *name);
int display_bind(struct display *disp);
void display_unbind(struct display *disp);
void display_ready(struct display *disp);

#define DISPLAY_CB(disp, act)                                                                      \
	shl_hook_call((disp)->hook, (disp),                                                        \
		      &(struct display_event){                                                     \
			      .action = (act),                                                     \
		      })

static inline bool display_is_online(const struct display *disp)
{
	return disp->video && (disp->flags & DISPLAY_ONLINE);
}

/* video */

#define VIDEO_AWAKE 0x01
#define VIDEO_HOTPLUG 0x02

struct video {
	unsigned long ref;
	unsigned int flags;
	struct ev_eloop *eloop;
	struct shl_register_record *record;

	struct shl_dlist displays;
	struct shl_hook *hook;

	bool use_original;
	unsigned int desired_width;
	unsigned int desired_height;

	const struct video_module *mod;
	void *data;
};

static inline bool video_need_hotplug(const struct video *video)
{
	return video->flags & VIDEO_HOTPLUG;
}

#define VIDEO_CB(vid, disp, act)                                                                   \
	shl_hook_call((vid)->hook, (vid),                                                          \
		      &(struct video_hotplug){                                                     \
			      .display = (disp),                                                   \
			      .action = (act),                                                     \
		      })
#endif /* VIDEO_INTERNAL_H */
