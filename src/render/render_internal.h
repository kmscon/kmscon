/*
 * kmscon - Render internals
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
 * Render internals
 * Shared by the render core and its backends only. Users of a render
 * renderer go through the functions of render.h.
 */

#ifndef KMSCON_RENDER_INTERNAL_H
#define KMSCON_RENDER_INTERNAL_H

#include <stdbool.h>
#include "render.h"

struct render {
	unsigned long ref;
	struct shl_register_record *record;
	const struct render_ops *ops;
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

struct render_cursor {
	struct tsm_cell cell;
	unsigned int x;
	unsigned int y;
	bool visible;
};

struct render_ops {
	const char *name;
	struct shl_module *owner;
	int (*init)(struct render *rdr);
	void (*destroy)(struct render *rdr);
	int (*set)(struct render *rdr);
	void (*unset)(struct render *rdr);
	void (*resize)(struct render *rdr, unsigned int cols, unsigned int rows);
	int (*rotate)(struct render *rdr, enum Orientation orientation);
	int (*prepare)(struct render *rdr, struct tsm_attr *attr);
	int (*draw)(struct render *rdr, const struct tsm_cell *cells, struct render_cursor *cursor);
	int (*draw_pointer)(struct render *rdr, unsigned int x, unsigned int y);
	int (*render)(struct render *rdr);
};

#define FONT_WIDTH(rdr) ((rdr)->font->width)
#define FONT_HEIGHT(rdr) ((rdr)->font->height)

/* modularized backends */

extern struct render_ops gltex_ops;

#endif /* KMSCON_TEXT_INTERNAL_H */
