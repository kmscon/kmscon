/*
 * kmscon - Text Renderer internals
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
 * Text Renderer internals
 * Shared by the text renderer core and its backends only. Users of a text
 * renderer go through the functions of text.h.
 */

#ifndef KMSCON_TEXT_INTERNAL_H
#define KMSCON_TEXT_INTERNAL_H

#include <stdbool.h>
#include "text.h"

struct kmscon_text {
	unsigned long ref;
	struct shl_register_record *record;
	const struct kmscon_text_ops *ops;
	void *data;

	struct kmscon_font *font;
	struct display *disp;
	unsigned int cols;
	unsigned int rows;
	unsigned int max_cols;
	unsigned int max_rows;
	bool rendering;
	enum Orientation orientation;
	bool blinking;
};

struct kmscon_cursor {
	struct tsm_screen_cell cell;
	unsigned int x;
	unsigned int y;
	bool visible;
};

struct kmscon_text_ops {
	const char *name;
	struct shl_module *owner;
	int (*init)(struct kmscon_text *txt);
	void (*destroy)(struct kmscon_text *txt);
	int (*set)(struct kmscon_text *txt);
	void (*unset)(struct kmscon_text *txt);
	void (*resize)(struct kmscon_text *txt, unsigned int cols, unsigned int rows);
	int (*rotate)(struct kmscon_text *txt, enum Orientation orientation);
	int (*prepare)(struct kmscon_text *txt, struct tsm_screen_attr *attr);
	int (*draw)(struct kmscon_text *txt, const struct tsm_screen_cell *cells,
		    struct kmscon_cursor *cursor);
	int (*draw_pointer)(struct kmscon_text *txt, unsigned int x, unsigned int y);
	int (*render)(struct kmscon_text *txt);
};

#define FONT_WIDTH(txt) ((txt)->font->width)
#define FONT_HEIGHT(txt) ((txt)->font->height)

/* modularized backends */

extern struct kmscon_text_ops kmscon_text_gltex_ops;

#endif /* KMSCON_TEXT_INTERNAL_H */
