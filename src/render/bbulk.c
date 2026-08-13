/*
 * kmscon - Bit-Blitting Bulk Text Renderer Backend
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

/**
 * SECTION:text_bbulk.c
 * @short_description: Bit-Blitting Bulk Text Renderer Backend
 * @include: text.h
 *
 * Similar to the bblit renderer but assembles an array of blit-requests and
 * pushes all of them at once to the video device.
 *
 * Only push cells that have changed from previous frame, and the frame before
 * as kmscon uses double buffering.
 * bbulk->prev holds the previous cell content, bbulk->damaged tells if the
 * previous cell content was different from its predecessor.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font/font.h"
#include "shl/log.h"
#include "shl/lru.h"
#include "shl/misc.h"
#include "text.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "text_bbulk"

#define ID_DAMAGED 0x80000001
#define ID_OVERFLOW 0x80000002

// Max horizontal distance of two damaged cells to be merged in a damage rectangle
#define DAMAGE_MERGE_LEN 3

/* Flags for each cell */
typedef union {
	struct {
		uint8_t double_width : 1;
		uint8_t damaged : 1;
	};
	uint8_t u8;
} cell_flags_t;

struct bbulk {
	unsigned int sw;	     /* screen width */
	unsigned int sh;	     /* screen height */
	unsigned int off_x;	     /* offset of the first cell */
	unsigned int off_y;	     /* offset of the first cell */
	unsigned int max_x;	     /* maximum x offset of the last cell */
	unsigned int max_y;	     /* maximum y offset of the last cell */
	struct tsm_screen_attr attr; /* attributes for background color */

	unsigned int requests; /* number of blend calls, for debugging */
	struct shl_lru *glyphs;
	struct tsm_screen_cell *cells;
	cell_flags_t *cell_flags;
	unsigned int cell_count;

	struct video_rect *damage_rects;
	unsigned int damage_rect_len;
	uint8_t redraw;
};

static int bbulk_init(struct kmscon_text *txt)
{
	struct bbulk *bb;

	bb = malloc(sizeof(*bb));
	if (!bb)
		return -ENOMEM;
	memset(bb, 0, sizeof(*bb));

	txt->data = bb;
	return 0;
}

static void bbulk_destroy(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	free(bb);
}

static void damage_cell(struct bbulk *bb, unsigned int off)
{
	bb->cells[off].ch = ID_DAMAGED;
	bb->cell_flags[off].damaged = 1;
}

static void compute_border(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		bb->off_x = (bb->sw - txt->cols * FONT_WIDTH(txt)) / 2;
		bb->off_y = (bb->sh - txt->rows * FONT_HEIGHT(txt)) / 2;
		bb->max_x = bb->off_x + txt->cols * FONT_WIDTH(txt);
		bb->max_y = bb->off_y + txt->rows * FONT_HEIGHT(txt);
	} else {
		bb->off_x = (bb->sw - txt->rows * FONT_HEIGHT(txt)) / 2;
		bb->off_y = (bb->sh - txt->cols * FONT_WIDTH(txt)) / 2;
		bb->max_x = bb->off_x + txt->rows * FONT_HEIGHT(txt);
		bb->max_y = bb->off_y + txt->cols * FONT_WIDTH(txt);
	}
	display_set_cursor_offset(txt->disp, bb->off_x, bb->off_y);
}

static int bbulk_set(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int max_damage_rects;
	int i;

	memset(bb, 0, sizeof(*bb));

	bb->sw = display_get_width(txt->disp);
	bb->sh = display_get_height(txt->disp);

	if (!bb->sw || !bb->sh)
		return -EINVAL;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		txt->max_cols = bb->sw / FONT_WIDTH(txt);
		txt->max_rows = bb->sh / FONT_HEIGHT(txt);
	} else {
		txt->max_rows = bb->sw / FONT_HEIGHT(txt);
		txt->max_cols = bb->sh / FONT_WIDTH(txt);
	}
	txt->cols = txt->max_cols;
	txt->rows = txt->max_rows;
	compute_border(txt);

	bb->cell_count = txt->max_cols * txt->max_rows;
	max_damage_rects = SHL_DIV_ROUND_UP(txt->max_cols, DAMAGE_MERGE_LEN + 1) * txt->max_rows;

	bb->cells = malloc(sizeof(*bb->cells) * bb->cell_count);
	if (!bb->cells)
		goto err_no_mem;
	memset(bb->cells, 0, sizeof(*bb->cells) * bb->cell_count);

	bb->cell_flags = malloc(sizeof(*bb->cell_flags) * bb->cell_count);
	if (!bb->cell_flags)
		goto free_prev;

	bb->damage_rects = malloc(sizeof(*bb->damage_rects) * max_damage_rects);
	if (!bb->damage_rects)
		goto free_damages;

	for (i = 0; i < (int)bb->cell_count; i++)
		damage_cell(bb, i);

	/* lru size should be at least bb->cells large */
	bb->glyphs = shl_lru_new(2 * bb->cell_count);
	if (!bb->glyphs)
		goto free_r_damages;
	return 0;

free_r_damages:
	free(bb->damage_rects);
free_damages:
	free(bb->cell_flags);
free_prev:
	free(bb->cells);
err_no_mem:
	return -ENOMEM;
}

static void bbulk_unset(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	shl_lru_free(bb->glyphs);
	free(bb->damage_rects);
	free(bb->cell_flags);
	free(bb->cells);
	bb->glyphs = NULL;
	bb->damage_rects = NULL;
	bb->cell_flags = NULL;
	bb->cells = NULL;
}

static void bbulk_resize(struct kmscon_text *txt, unsigned int cols, unsigned int rows)
{
	struct bbulk *bb = txt->data;

	txt->cols = cols;
	txt->rows = rows;
	compute_border(txt);
	bb->redraw = 2;
}

static int bbulk_rotate(struct kmscon_text *txt, enum Orientation orientation)
{
	bbulk_unset(txt);
	txt->orientation = orientation;
	return bbulk_set(txt);
}

/*
 * Rotate a glyph to the given orientation
 * Return a new rotated glyph, and free the original glyph
 */
static struct kmscon_glyph *bbulk_rotate_glyph(struct kmscon_glyph *glyph,
					       enum Orientation orientation)
{
	struct video_buffer *buf = &glyph->buf;
	struct kmscon_glyph *rglyph;
	int width, height, i, j;
	uint8_t *dst, *src;
	unsigned int size = sizeof(*rglyph) + glyph->buf.width * glyph->buf.height;

	rglyph = malloc(size);
	if (!rglyph)
		goto err_free;

	memset(rglyph, 0, size);

	if (orientation == OR_NORMAL || orientation == OR_UPSIDE_DOWN) {
		width = buf->width;
		height = buf->height;
	} else {
		width = buf->height;
		height = buf->width;
	}
	src = buf->data;
	dst = rglyph->buf.data;

	switch (orientation) {
	default:
	case OR_NORMAL:
		/* should never happen */
		break;
	case OR_RIGHT:
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++) {
				dst[j * width + (width - i - 1)] = src[j];
			}
			src += buf->stride;
		}
		break;
	case OR_UPSIDE_DOWN:
		src += (buf->height - 1) * buf->stride;
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++)
				dst[j] = src[buf->width - j - 1];
			dst += width;
			src -= buf->stride;
		}
		break;
	case OR_LEFT:
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++) {
				dst[(height - j - 1) * width + i] = src[j];
			}
			src += buf->stride;
		}
	}
	rglyph->buf.width = width;
	rglyph->buf.height = height;
	rglyph->buf.stride = width;
	rglyph->double_width = glyph->double_width;

err_free:
	free(glyph);
	return rglyph;
}

static struct kmscon_glyph *find_glyph(struct kmscon_text *txt, const struct tsm_screen_cell *cell)
{
	struct bbulk *bb = txt->data;
	struct kmscon_glyph *glyph;
	struct kmscon_font *font = txt->font;
	uint32_t ch = cell->ch ? cell->ch : ' ';
	uint64_t id;

	font->attr.underline = !!cell->attr2.underline;
	font->attr.italic = !!cell->attr2.italic;
	font->attr.bold = !!cell->attr2.bold;

	if (!kmscon_font_has_glyph(font, ch))
		ch = 0x25a1;

	id = kmscon_glyph_id(cell->ch, cell->attr2.u8);

	glyph = shl_lru_get(bb->glyphs, id);
	if (glyph)
		return glyph;

	glyph = kmscon_font_render(font, ch);
	if (!glyph)
		return NULL;

	if (txt->orientation != OR_NORMAL)
		glyph = bbulk_rotate_glyph(glyph, txt->orientation);

	if (shl_lru_insert(bb->glyphs, id, glyph)) {
		free(glyph);
		return NULL;
	}
	return glyph;
}

/*
 * Returns the top left corner of a Cell
 */
static void set_coordinate(struct kmscon_text *txt, unsigned int *x, unsigned int *y,
			   unsigned int posx, unsigned int posy)
{
	struct bbulk *bb = txt->data;

	switch (txt->orientation) {
	case OR_NORMAL:
		*x = posx * FONT_WIDTH(txt) + bb->off_x;
		*y = posy * FONT_HEIGHT(txt) + bb->off_y;
		break;
	case OR_UPSIDE_DOWN:
		*x = bb->max_x - (posx + 1) * FONT_WIDTH(txt);
		*y = bb->max_y - (posy + 1) * FONT_HEIGHT(txt);
		break;
	case OR_RIGHT:
		*x = bb->max_x - (posy + 1) * FONT_HEIGHT(txt);
		*y = posx * FONT_WIDTH(txt) + bb->off_y;
		break;
	case OR_LEFT:
		*x = posy * FONT_HEIGHT(txt) + bb->off_x;
		*y = bb->max_y - (posx + 1) * FONT_WIDTH(txt);
		break;
	}
}

static void set_color(struct video_blend_req *req, const struct tsm_screen_cell *cell)
{
	req->fr = cell->fg.r;
	req->fg = cell->fg.g;
	req->fb = cell->fg.b;
	req->br = cell->bg.r;
	req->bg = cell->bg.g;
	req->bb = cell->bg.b;
}

static int bbulk_draw_cell(struct kmscon_text *txt, const struct tsm_screen_cell *cell,
			   unsigned int posx, unsigned int posy)
{
	struct bbulk *bb = txt->data;
	unsigned int offset = posx + posy * txt->cols;
	struct tsm_screen_cell *cur_cell = &bb->cells[offset];
	struct kmscon_glyph *glyph;
	struct video_blend_req req;
	bool last_col = (posx == txt->cols - 1);
	bool changed;

	// left cell overflow on this cell.
	if ((cell->ch == 0 || cell->ch == ' ') && posx && bb->cell_flags[offset - 1].double_width) {
		if (cur_cell->ch == ID_OVERFLOW) {
			bb->cell_flags[offset].damaged = 0;
			return 0;
		}
		cur_cell->ch = ID_OVERFLOW;
		bb->cell_flags[offset].damaged = 1;
		return 0;
	}

	changed = memcmp(cur_cell, cell, sizeof(*cell));

	if (!changed) {
		/* Cell content is unchanged */
		if (bb->cell_flags[offset].double_width && !last_col) {
			if (!bb->cell_flags[offset].damaged && !bb->cell_flags[offset + 1].damaged)
				return 0;
			bb->cell_flags[offset].damaged = 0;
		} else {
			if (!bb->cell_flags[offset].damaged)
				return 0;
			bb->cell_flags[offset].damaged = 0;
		}
	} else {
		bb->cell_flags[offset].damaged = 1;
	}

	*cur_cell = *cell;

	glyph = find_glyph(txt, cur_cell);
	if (!glyph)
		return -ENOMEM;

	if (glyph->double_width && !last_col && changed)
		damage_cell(bb, offset + 1);

	bb->cell_flags[offset].double_width = glyph->double_width;

	if (glyph->double_width && !last_col &&
	    (txt->orientation == OR_LEFT || txt->orientation == OR_UPSIDE_DOWN))
		/*
		 * In case of left or upside down orientation, we need to draw to the
		 * next cell, as the glyph is already rotated, so start on the next cell
		 * and end on this cell
		 */
		set_coordinate(txt, &req.x, &req.y, posx + 1, posy);
	else
		set_coordinate(txt, &req.x, &req.y, posx, posy);

	req.w = glyph->buf.width;
	req.h = glyph->buf.height;

	/* Truncate the glyph if it would overflow in the border */
	if (glyph->double_width && last_col) {
		if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN)
			req.w = FONT_WIDTH(txt);
		else if (txt->orientation == OR_RIGHT || txt->orientation == OR_LEFT)
			req.h = FONT_WIDTH(txt);
	}

	req.buf = &glyph->buf;
	set_color(&req, cur_cell);
	display_blend(txt->disp, &req);
	bb->requests++;
	return 0;
}

static int bbulk_draw(struct kmscon_text *txt, const struct tsm_screen_cell *cells,
		      struct kmscon_cursor *cursor)
{
	unsigned int posx, posy, off;

	for (posy = 0; posy < txt->rows; posy++) {
		for (posx = 0; posx < txt->cols; posx++) {
			off = posx + posy * txt->cols;

			if (cursor->visible && cursor->x == posx && cursor->y == posy)
				bbulk_draw_cell(txt, &cursor->cell, posx, posy);
			else if (cells[off].attr2.blink && txt->blinking) {
				struct tsm_screen_cell cell = cells[off];

				cell.ch = ' ';
				bbulk_draw_cell(txt, &cell, posx, posy);
			} else
				bbulk_draw_cell(txt, &cells[off], posx, posy);
		}
	}
	return 0;
}

/*
 * When the pointer move over, mark the 4 underlying cells as damaged.
 */
static void mark_damaged(struct kmscon_text *txt, struct bbulk *bb, unsigned int x, unsigned int y)
{
	unsigned int posx = 0;
	unsigned int posy = 0;
	unsigned int fw, fh, off;
	fw = SHL_DIV_ROUND_UP(FONT_WIDTH(txt), 2);
	fh = SHL_DIV_ROUND_UP(FONT_HEIGHT(txt), 2);

	if (x > fw)
		posx = (x - fw) / FONT_WIDTH(txt);
	if (y > fh)
		posy = (y - fh) / FONT_HEIGHT(txt);

	if (posx >= txt->cols)
		posx = txt->cols - 1;

	if (posy >= txt->rows)
		posy = txt->rows - 1;

	off = posx + posy * txt->cols;

	damage_cell(bb, off);

	if (posx + 1 < txt->cols)
		damage_cell(bb, off + 1);

	if (posy + 1 < txt->rows)
		damage_cell(bb, off + txt->cols);

	if (posx + 1 < txt->cols && posy + 1 < txt->rows)
		damage_cell(bb, off + 1 + txt->cols);
}

static unsigned int clamp(unsigned int val, unsigned int min, unsigned int max)
{
	if (val < min)
		return min;
	if (val > max)
		return max;
	return val;
}

/*
 * pointer_x and pointer_y are the center of the pointer sprite, in the
 * non-rotated screen.
 */
static void set_pointer_coordinate(struct bbulk *bb, struct kmscon_text *txt,
				   struct video_blend_req *req, unsigned int pointer_x,
				   unsigned int pointer_y)
{
	unsigned int hf_w, hf_h, x, y;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		hf_w = SHL_DIV_ROUND_UP(FONT_WIDTH(txt), 2);
		hf_h = SHL_DIV_ROUND_UP(FONT_HEIGHT(txt), 2);
	} else {
		hf_w = SHL_DIV_ROUND_UP(FONT_HEIGHT(txt), 2);
		hf_h = SHL_DIV_ROUND_UP(FONT_WIDTH(txt), 2);
	}

	switch (txt->orientation) {
	default:
	case OR_NORMAL:
		x = pointer_x;
		y = pointer_y;
		break;
	case OR_UPSIDE_DOWN:
		x = bb->sw - pointer_x;
		y = bb->sh - pointer_y;
		break;
	case OR_RIGHT:
		x = bb->sw - pointer_y;
		y = pointer_x;
		break;
	case OR_LEFT:
		x = pointer_y;
		y = bb->sh - pointer_x;
		break;
	}
	x = clamp(x, hf_w, bb->sw - hf_w);
	y = clamp(y, hf_h, bb->sh - hf_h);

	x += bb->off_x - hf_w;
	y += bb->off_y - hf_h;

	req->x = x;
	req->y = y;
}

static int bbulk_draw_pointer(struct kmscon_text *txt, unsigned int pointer_x,
			      unsigned int pointer_y)
{
	struct bbulk *bb = txt->data;
	struct video_blend_req req;
	struct kmscon_glyph *bb_glyph;
	struct tsm_screen_cell pointer_cell = {0};
	pointer_cell.ch = 'I';

	pointer_x = min(pointer_x, txt->cols * FONT_WIDTH(txt) - (FONT_WIDTH(txt) / 2));
	pointer_y = min(pointer_y, txt->rows * FONT_HEIGHT(txt) - (FONT_HEIGHT(txt) / 2));

	mark_damaged(txt, bb, pointer_x, pointer_y);

	bb_glyph = find_glyph(txt, &pointer_cell);
	if (!bb_glyph)
		return -ENOMEM;

	req.buf = &bb_glyph->buf;
	req.w = bb_glyph->buf.width;
	req.h = bb_glyph->buf.height;
	set_pointer_coordinate(bb, txt, &req, pointer_x, pointer_y);

	req.fr = bb->attr.fr;
	req.fg = bb->attr.fg;
	req.fb = bb->attr.fb;
	req.br = bb->attr.br;
	req.bg = bb->attr.bg;
	req.bb = bb->attr.bb;
	display_blend(txt->disp, &req);
	bb->requests++;
	return 0;
}

static void add_damage(struct bbulk *bb, struct video_rect *r)
{
	struct video_rect *out = &bb->damage_rects[bb->damage_rect_len];

	*out = *r;
	bb->damage_rect_len++;
}

static void merge_damage(struct bbulk *bb, struct video_rect *r)
{
	struct video_rect *out = &bb->damage_rects[bb->damage_rect_len - 1];

	out->x1 = min(out->x1, r->x1);
	out->x2 = max(out->x2, r->x2);
	out->y1 = min(out->y1, r->y1);
	out->y2 = max(out->y2, r->y2);
}

/*
 * Simple merge algorithm, on each line, if two damaged cells are less than
 * DAMAGE_MERGE_LEN away, include the two cells in one damage rectangle.
 */
static void bbulk_compute_damage(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int posx, posy, off;
	struct video_rect r;
	unsigned int x1 = 0, y1 = 0;
	unsigned int fw, fh;
	int prev;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		fw = FONT_WIDTH(txt);
		fh = FONT_HEIGHT(txt);
	} else {
		fw = FONT_HEIGHT(txt);
		fh = FONT_WIDTH(txt);
	}

	for (posy = 0; posy < txt->rows; posy++) {
		prev = 0;
		for (posx = 0; posx < txt->cols; posx++) {
			off = posx + posy * txt->cols;
			if (!bb->cell_flags[off].damaged) {
				if (prev)
					prev--;
				continue;
			}
			set_coordinate(txt, &x1, &y1, posx, posy);
			r.x1 = x1;
			r.y1 = y1;
			r.x2 = x1 + fw;
			r.y2 = y1 + fh;
			if (prev)
				merge_damage(bb, &r);
			else
				add_damage(bb, &r);
			prev = DAMAGE_MERGE_LEN;
		}
	}
}
static int bbulk_render(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int ret = 0;

	// log_debug("bbulk, redraw %d cells", bb->requests);
	if (display_supports_damage(txt->disp)) {
		bbulk_compute_damage(txt);
		display_set_damage(txt->disp, bb->damage_rect_len, bb->damage_rects);
	}
	return ret;
}

static int bbulk_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr)
{
	struct bbulk *bb = txt->data;
	int i;

	bb->requests = 0;
	bb->damage_rect_len = 0;

	/*
	 * if default colors have changed, or we switch from a dirty screen,
	 * redraw completely the next 2 frames.
	 */
	if (memcmp(&bb->attr, attr, sizeof(*attr)) || display_need_redraw(txt->disp))
		bb->redraw = 2;

	bb->attr = *attr;

	if (bb->redraw) {
		display_clear(txt->disp, attr->br, attr->bg, attr->bb);
		for (i = 0; i < bb->cell_count; i++)
			damage_cell(bb, i);
	} else if (display_has_damage(txt->disp)) {
		log_debug("Carry over damage from previous frame");
		for (i = 0; i < bb->cell_count; i++) {
			if (bb->cell_flags[i].damaged)
				bb->cells[i].ch = ID_DAMAGED;
		}
	}
	if (bb->redraw)
		bb->redraw--;

	return 0;
}

struct kmscon_text_ops kmscon_text_bbulk_ops = {
	.name = "bbulk",
	.owner = NULL,
	.init = bbulk_init,
	.destroy = bbulk_destroy,
	.set = bbulk_set,
	.unset = bbulk_unset,
	.resize = bbulk_resize,
	.rotate = bbulk_rotate,
	.prepare = bbulk_prepare,
	.draw = bbulk_draw,
	.draw_pointer = bbulk_draw_pointer,
	.render = bbulk_render,
	.abort = NULL,
};
