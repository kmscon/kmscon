/*
 * kmscon - Render
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * Render
 * The Render subsystem provides a simple way to draw text into a
 * framebuffer. The system is modular and several different backends are
 * available that can be used.
 */

#ifndef KMSCON_RENDER_H
#define KMSCON_RENDER_H

#include <errno.h>
#include <stdint.h>
#include "font/font.h"
#include "tsm.h"
#include "video/video.h"

/* render */

enum Orientation {
	OR_NORMAL = 0,	// 0 Degree
	OR_RIGHT,	// 90 Degree
	OR_UPSIDE_DOWN, // 180 Degree
	OR_LEFT,	// 270 Degree
};

struct render_font;
struct render;
struct render_ops;

int render_register(const struct render_ops *ops);
void render_unregister(const struct render_ops *ops);

int render_new(struct render **out, const char *backend, enum Orientation orientation,
	       struct display *disp);
void render_ref(struct render *rdr);
void render_unref(struct render *rdr);

int render_set(struct render *rdr, struct kmscon_font *font);
void render_unset(struct render *rdr);

unsigned int render_get_width(struct render *rdr);
unsigned int render_get_height(struct render *rdr);
unsigned int render_get_cols(struct render *rdr, unsigned int font_width);
unsigned int render_get_rows(struct render *rdr, unsigned int font_height);

enum Orientation render_get_orientation(struct render *rdr);
const char *render_get_name(struct render *rdr);
unsigned int render_get_font_width(struct render *rdr);
unsigned int render_get_font_height(struct render *rdr);
void render_resize(struct render *rdr, unsigned int cols, unsigned int rows);
int render_rotate(struct render *rdr, enum Orientation orientation);

int render_prepare(struct render *rdr, struct tsm_attr *attr, bool blinking);
int render_draw(struct render *rdr, struct tsm_vte *vte, bool cursor_blink);
int render_draw_pointer(struct render *rdr, unsigned int x, unsigned int y);
int render_render(struct render *rdr);
void render_show_cursor(struct render *rdr, int32_t x, int32_t y);

/* built-in backend */

extern struct render_ops render_bbulk_ops;

#endif /* KMSCON_TEXT_H */
