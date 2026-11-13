/*
 * kmscon - Text Renderer
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
 * Text Renderer
 * The Text-Renderer subsystem provides a simple way to draw text into a
 * framebuffer. The system is modular and several different backends are
 * available that can be used.
 */

#ifndef KMSCON_TEXT_H
#define KMSCON_TEXT_H

#include <errno.h>
#include <libtsm.h>
#include <stdint.h>
#include "font/font.h"
#include "video/video.h"

/* text renderer */

enum Orientation {
	OR_NORMAL = 0,	// 0 Degree
	OR_RIGHT,	// 90 Degree
	OR_UPSIDE_DOWN, // 180 Degree
	OR_LEFT,	// 270 Degree
};

struct kmscon_font;
struct kmscon_text;
struct kmscon_text_ops;

int kmscon_text_register(const struct kmscon_text_ops *ops);
void kmscon_text_unregister(const struct kmscon_text_ops *ops);

int kmscon_text_new(struct kmscon_text **out, const char *backend, enum Orientation orientation,
		    struct display *disp);
void kmscon_text_ref(struct kmscon_text *txt);
void kmscon_text_unref(struct kmscon_text *txt);

int kmscon_text_set(struct kmscon_text *txt, struct kmscon_font *font);
void kmscon_text_unset(struct kmscon_text *txt);

unsigned int kmscon_text_get_width(struct kmscon_text *txt);
unsigned int kmscon_text_get_height(struct kmscon_text *txt);
unsigned int kmscon_text_get_cols(struct kmscon_text *txt, unsigned int font_width);
unsigned int kmscon_text_get_rows(struct kmscon_text *txt, unsigned int font_height);

enum Orientation kmscon_text_get_orientation(struct kmscon_text *txt);
const char *kmscon_text_get_name(struct kmscon_text *txt);
unsigned int kmscon_text_get_font_width(struct kmscon_text *txt);
unsigned int kmscon_text_get_font_height(struct kmscon_text *txt);
void kmscon_text_resize(struct kmscon_text *txt, unsigned int cols, unsigned int rows);
int kmscon_text_rotate(struct kmscon_text *txt, enum Orientation orientation);

int kmscon_text_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr, bool blinking);
int kmscon_text_draw(struct kmscon_text *txt, struct tsm_screen *con, bool cursor_blink);
int kmscon_text_draw_pointer(struct kmscon_text *txt, unsigned int x, unsigned int y);
int kmscon_text_render(struct kmscon_text *txt);
void kmscon_text_abort(struct kmscon_text *txt);
void kmscon_text_show_cursor(struct kmscon_text *txt, int32_t x, int32_t y);

/* built-in backend */

extern struct kmscon_text_ops kmscon_text_bbulk_ops;

#endif /* KMSCON_TEXT_H */
