/*
 * kmscon - Bit-Blitting Bulk Render Backend
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
 * SECTION:render_bbulk.c
 * @short_description: Bit-Blitting Bulk Render Backend
 * @include: render_internal.h
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
#include "render_internal.h"
#include "shl/log.h"
#include "shl/lru.h"
#include "shl/misc.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "render_bbulk"

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
	unsigned int sw;      /* screen width */
	unsigned int sh;      /* screen height */
	unsigned int off_x;   /* offset of the first cell */
	unsigned int off_y;   /* offset of the first cell */
	unsigned int max_x;   /* maximum x offset of the last cell */
	unsigned int max_y;   /* maximum y offset of the last cell */
	struct tsm_attr attr; /* attributes for background color */

	unsigned int requests; /* number of blend calls, for debugging */
	struct shl_lru *glyphs;
	struct tsm_cell *cells;
	cell_flags_t *cell_flags;
	unsigned int cell_count;

	struct video_rect *damage_rects;
	unsigned int damage_rect_len;
	uint8_t redraw;
};

static int bbulk_init(struct render *rdr)
{
	struct bbulk *bb;

	bb = calloc(1, sizeof(*bb));
	if (!bb)
		return -ENOMEM;

	rdr->data = bb;
	return 0;
}

static void bbulk_destroy(struct render *rdr)
{
	struct bbulk *bb = rdr->data;

	free(bb);
}

static void damage_cell(struct bbulk *bb, unsigned int off)
{
	bb->cells[off].ch = ID_DAMAGED;
	bb->cell_flags[off].damaged = 1;
}

static void compute_border(struct render *rdr)
{
	struct bbulk *bb = rdr->data;

	if (rdr->orientation == OR_NORMAL || rdr->orientation == OR_UPSIDE_DOWN) {
		bb->off_x = (bb->sw - rdr->cols * FONT_WIDTH(rdr)) / 2;
		bb->off_y = (bb->sh - rdr->rows * FONT_HEIGHT(rdr)) / 2;
		bb->max_x = bb->off_x + rdr->cols * FONT_WIDTH(rdr);
		bb->max_y = bb->off_y + rdr->rows * FONT_HEIGHT(rdr);
	} else {
		bb->off_x = (bb->sw - rdr->rows * FONT_HEIGHT(rdr)) / 2;
		bb->off_y = (bb->sh - rdr->cols * FONT_WIDTH(rdr)) / 2;
		bb->max_x = bb->off_x + rdr->rows * FONT_HEIGHT(rdr);
		bb->max_y = bb->off_y + rdr->cols * FONT_WIDTH(rdr);
	}
	display_set_cursor_offset(rdr->disp, bb->off_x, bb->off_y);
}

static int bbulk_set(struct render *rdr)
{
	struct bbulk *bb = rdr->data;
	int max_damage_rects;
	int i;

	memset(bb, 0, sizeof(*bb));

	bb->sw = display_get_width(rdr->disp);
	bb->sh = display_get_height(rdr->disp);

	if (!bb->sw || !bb->sh)
		return -EINVAL;

	rdr->max_cols = render_get_cols(rdr, FONT_WIDTH(rdr));
	rdr->max_rows = render_get_rows(rdr, FONT_HEIGHT(rdr));
	rdr->cols = rdr->max_cols;
	rdr->rows = rdr->max_rows;
	compute_border(rdr);

	bb->cell_count = rdr->max_cols * rdr->max_rows;
	max_damage_rects = SHL_DIV_ROUND_UP(rdr->max_cols, DAMAGE_MERGE_LEN + 1) * rdr->max_rows;

	bb->cells = calloc(bb->cell_count, sizeof(*bb->cells));
	if (!bb->cells)
		goto err_no_mem;

	bb->cell_flags = calloc(bb->cell_count, sizeof(*bb->cell_flags));
	if (!bb->cell_flags)
		goto free_prev;

	bb->damage_rects = calloc(max_damage_rects, sizeof(*bb->damage_rects));
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

static void bbulk_unset(struct render *rdr)
{
	struct bbulk *bb = rdr->data;

	shl_lru_free(bb->glyphs);
	free(bb->damage_rects);
	free(bb->cell_flags);
	free(bb->cells);
	bb->glyphs = NULL;
	bb->damage_rects = NULL;
	bb->cell_flags = NULL;
	bb->cells = NULL;
}

static void bbulk_resize(struct render *rdr, unsigned int cols, unsigned int rows)
{
	struct bbulk *bb = rdr->data;

	rdr->cols = cols;
	rdr->rows = rows;
	compute_border(rdr);
	bb->redraw = 2;
}

static int bbulk_rotate(struct render *rdr, enum Orientation orientation)
{
	bbulk_unset(rdr);
	rdr->orientation = orientation;
	return bbulk_set(rdr);
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

	rglyph = calloc(1, sizeof(*rglyph) + glyph->buf.width * glyph->buf.height);
	if (!rglyph)
		goto err_free;

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
			src += buf->width;
		}
		break;
	case OR_UPSIDE_DOWN:
		src += (buf->height - 1) * buf->width;
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++)
				dst[j] = src[buf->width - j - 1];
			dst += width;
			src -= buf->width;
		}
		break;
	case OR_LEFT:
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++) {
				dst[(height - j - 1) * width + i] = src[j];
			}
			src += buf->width;
		}
	}
	rglyph->buf.width = width;
	rglyph->buf.height = height;
	rglyph->double_width = glyph->double_width;

err_free:
	free(glyph);
	return rglyph;
}

static struct kmscon_glyph *find_glyph(struct render *rdr, const struct tsm_cell *cell)
{
	struct bbulk *bb = rdr->data;
	struct kmscon_glyph *glyph;
	struct kmscon_font *font = rdr->font;
	struct kmscon_font_attr attr;
	uint32_t ch = cell->ch ? cell->ch : ' ';
	uint64_t id;

	attr.underline = !!(cell->attr & TSM_ATTR_UNDERLINE);
	attr.italic = !!(cell->attr & TSM_ATTR_ITALIC);
	attr.bold = !!(cell->attr & TSM_ATTR_BOLD);

	if (!kmscon_font_has_glyph(font, &attr, ch))
		ch = FONT_REPLACEMENT_CHAR;

	id = kmscon_glyph_id(cell->ch, cell->attr);

	glyph = shl_lru_get(bb->glyphs, id);
	if (glyph)
		return glyph;

	glyph = kmscon_font_render(font, &attr, ch);
	if (!glyph)
		return NULL;

	if (rdr->orientation != OR_NORMAL)
		glyph = bbulk_rotate_glyph(glyph, rdr->orientation);

	if (shl_lru_insert(bb->glyphs, id, glyph)) {
		free(glyph);
		return NULL;
	}
	return glyph;
}

/*
 * Returns the top left corner of a Cell
 */
static void set_coordinate(struct render *rdr, unsigned int *x, unsigned int *y, unsigned int posx,
			   unsigned int posy)
{
	struct bbulk *bb = rdr->data;

	switch (rdr->orientation) {
	case OR_NORMAL:
		*x = posx * FONT_WIDTH(rdr) + bb->off_x;
		*y = posy * FONT_HEIGHT(rdr) + bb->off_y;
		break;
	case OR_UPSIDE_DOWN:
		*x = bb->max_x - (posx + 1) * FONT_WIDTH(rdr);
		*y = bb->max_y - (posy + 1) * FONT_HEIGHT(rdr);
		break;
	case OR_RIGHT:
		*x = bb->max_x - (posy + 1) * FONT_HEIGHT(rdr);
		*y = posx * FONT_WIDTH(rdr) + bb->off_y;
		break;
	case OR_LEFT:
		*x = posy * FONT_HEIGHT(rdr) + bb->off_x;
		*y = bb->max_y - (posx + 1) * FONT_WIDTH(rdr);
		break;
	}
}

static void set_color(struct video_blend_req *req, const struct tsm_cell *cell)
{
	req->fr = cell->fg.r;
	req->fg = cell->fg.g;
	req->fb = cell->fg.b;
	req->br = cell->bg.r;
	req->bg = cell->bg.g;
	req->bb = cell->bg.b;
}

static int bbulk_draw_cell(struct render *rdr, const struct tsm_cell *cell, unsigned int posx,
			   unsigned int posy)
{
	struct bbulk *bb = rdr->data;
	unsigned int offset = posx + posy * rdr->cols;
	struct tsm_cell *cur_cell = &bb->cells[offset];
	struct kmscon_glyph *glyph;
	struct video_blend_req req;
	bool last_col = (posx == rdr->cols - 1);
	bool changed;

	// left cell overflow on this cell.
	if ((cell->ch == 0 || cell->ch == ' ') && posx && bb->cell_flags[offset - 1].double_width) {
		/* an overflow cell never starts a wide glyph: drop the stale flag */
		bb->cell_flags[offset].double_width = 0;
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

	glyph = find_glyph(rdr, cur_cell);
	if (!glyph)
		return -ENOMEM;

	if (glyph->double_width && !last_col && changed)
		damage_cell(bb, offset + 1);

	bb->cell_flags[offset].double_width = glyph->double_width;

	if (glyph->double_width && !last_col &&
	    (rdr->orientation == OR_LEFT || rdr->orientation == OR_UPSIDE_DOWN))
		/*
		 * In case of left or upside down orientation, we need to draw to the
		 * next cell, as the glyph is already rotated, so start on the next cell
		 * and end on this cell
		 */
		set_coordinate(rdr, &req.x, &req.y, posx + 1, posy);
	else
		set_coordinate(rdr, &req.x, &req.y, posx, posy);

	req.w = glyph->buf.width;
	req.h = glyph->buf.height;

	/* Truncate the glyph if it would overflow in the border */
	if (glyph->double_width && last_col) {
		if (rdr->orientation == OR_NORMAL || rdr->orientation == OR_UPSIDE_DOWN)
			req.w = FONT_WIDTH(rdr);
		else if (rdr->orientation == OR_RIGHT || rdr->orientation == OR_LEFT)
			req.h = FONT_WIDTH(rdr);
	}

	req.buf = &glyph->buf;
	set_color(&req, cur_cell);
	display_blend(rdr->disp, &req);
	bb->requests++;
	return 0;
}

static int bbulk_draw(struct render *rdr, const struct tsm_cell *cells,
		      struct render_cursor *cursor)
{
	unsigned int posx, posy, off;

	for (posy = 0; posy < rdr->rows; posy++) {
		for (posx = 0; posx < rdr->cols; posx++) {
			off = posx + posy * rdr->cols;

			if (cursor->visible && cursor->x == posx && cursor->y == posy)
				bbulk_draw_cell(rdr, &cursor->cell, posx, posy);
			else if ((cells[off].attr & TSM_ATTR_BLINK) && rdr->blinking) {
				struct tsm_cell cell = cells[off];

				cell.ch = ' ';
				bbulk_draw_cell(rdr, &cell, posx, posy);
			} else
				bbulk_draw_cell(rdr, &cells[off], posx, posy);
		}
	}
	return 0;
}

/*
 * When the pointer move over, mark the 4 underlying cells as damaged.
 */
static void mark_damaged(struct render *rdr, struct bbulk *bb, unsigned int x, unsigned int y)
{
	unsigned int posx = 0;
	unsigned int posy = 0;
	unsigned int fw, fh, off;
	fw = SHL_DIV_ROUND_UP(FONT_WIDTH(rdr), 2);
	fh = SHL_DIV_ROUND_UP(FONT_HEIGHT(rdr), 2);

	if (x > fw)
		posx = (x - fw) / FONT_WIDTH(rdr);
	if (y > fh)
		posy = (y - fh) / FONT_HEIGHT(rdr);

	if (posx >= rdr->cols)
		posx = rdr->cols - 1;

	if (posy >= rdr->rows)
		posy = rdr->rows - 1;

	off = posx + posy * rdr->cols;

	damage_cell(bb, off);

	if (posx + 1 < rdr->cols)
		damage_cell(bb, off + 1);

	if (posy + 1 < rdr->rows)
		damage_cell(bb, off + rdr->cols);

	if (posx + 1 < rdr->cols && posy + 1 < rdr->rows)
		damage_cell(bb, off + 1 + rdr->cols);
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
static void set_pointer_coordinate(struct bbulk *bb, struct render *rdr,
				   struct video_blend_req *req, unsigned int pointer_x,
				   unsigned int pointer_y)
{
	unsigned int hf_w, hf_h, x, y;

	if (rdr->orientation == OR_NORMAL || rdr->orientation == OR_UPSIDE_DOWN) {
		hf_w = SHL_DIV_ROUND_UP(FONT_WIDTH(rdr), 2);
		hf_h = SHL_DIV_ROUND_UP(FONT_HEIGHT(rdr), 2);
	} else {
		hf_w = SHL_DIV_ROUND_UP(FONT_HEIGHT(rdr), 2);
		hf_h = SHL_DIV_ROUND_UP(FONT_WIDTH(rdr), 2);
	}

	switch (rdr->orientation) {
	default:
	case OR_NORMAL:
		x = pointer_x + bb->off_x;
		y = pointer_y + bb->off_y;
		break;
	case OR_UPSIDE_DOWN:
		x = bb->max_x - pointer_x;
		y = bb->max_y - pointer_y;
		break;
	case OR_RIGHT:
		x = bb->max_x - pointer_y;
		y = pointer_x + bb->off_y;
		break;
	case OR_LEFT:
		x = pointer_y + bb->off_x;
		y = bb->max_y - pointer_x;
		break;
	}
	x = clamp(x, hf_w, bb->max_x - hf_w);
	y = clamp(y, hf_h, bb->max_y - hf_h);

	req->x = x - hf_w;
	req->y = y - hf_h;
}

static int bbulk_draw_pointer(struct render *rdr, unsigned int pointer_x, unsigned int pointer_y)
{
	struct bbulk *bb = rdr->data;
	struct video_blend_req req;
	struct kmscon_glyph *bb_glyph;
	struct tsm_cell pointer_cell = {0};
	unsigned int fw2 = FONT_WIDTH(rdr) / 2;
	unsigned int fh2 = FONT_HEIGHT(rdr) / 2;

	pointer_cell.ch = 'I';

	pointer_x = clamp(pointer_x, fw2, rdr->cols * FONT_WIDTH(rdr) - fw2);
	pointer_y = clamp(pointer_y, fh2, rdr->rows * FONT_HEIGHT(rdr) - fh2);

	bb_glyph = find_glyph(rdr, &pointer_cell);
	if (!bb_glyph)
		return -ENOMEM;

	req.buf = &bb_glyph->buf;
	req.w = bb_glyph->buf.width;
	req.h = bb_glyph->buf.height;
	set_pointer_coordinate(bb, rdr, &req, pointer_x, pointer_y);
	mark_damaged(rdr, bb, pointer_x, pointer_y);

	req.fr = bb->attr.fg.r;
	req.fg = bb->attr.fg.g;
	req.fb = bb->attr.fg.b;
	req.br = bb->attr.bg.r;
	req.bg = bb->attr.bg.g;
	req.bb = bb->attr.bg.b;
	display_blend(rdr->disp, &req);
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
static void bbulk_compute_damage(struct render *rdr)
{
	struct bbulk *bb = rdr->data;
	int posx, posy, off;
	struct video_rect r;
	unsigned int x1 = 0, y1 = 0;
	unsigned int fw, fh;
	int prev;

	if (rdr->orientation == OR_NORMAL || rdr->orientation == OR_UPSIDE_DOWN) {
		fw = FONT_WIDTH(rdr);
		fh = FONT_HEIGHT(rdr);
	} else {
		fw = FONT_HEIGHT(rdr);
		fh = FONT_WIDTH(rdr);
	}

	for (posy = 0; posy < rdr->rows; posy++) {
		prev = 0;
		for (posx = 0; posx < rdr->cols; posx++) {
			off = posx + posy * rdr->cols;
			if (!bb->cell_flags[off].damaged) {
				if (prev)
					prev--;
				continue;
			}
			set_coordinate(rdr, &x1, &y1, posx, posy);
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
static int bbulk_render(struct render *rdr)
{
	struct bbulk *bb = rdr->data;
	int ret = 0;

	// log_debug("bbulk, redraw %d cells", bb->requests);
	if (display_supports_damage(rdr->disp)) {
		bbulk_compute_damage(rdr);
		display_set_damage(rdr->disp, bb->damage_rect_len, bb->damage_rects);
	}
	return ret;
}

static int bbulk_prepare(struct render *rdr, struct tsm_attr *attr)
{
	struct bbulk *bb = rdr->data;
	int i;

	bb->requests = 0;
	bb->damage_rect_len = 0;

	/*
	 * if default colors have changed, or we switch from a dirty screen,
	 * redraw completely the next 2 frames.
	 */
	if (memcmp(&bb->attr, attr, sizeof(*attr)) || display_need_redraw(rdr->disp))
		bb->redraw = 2;

	bb->attr = *attr;

	if (bb->redraw) {
		display_clear(rdr->disp, attr->bg.r, attr->bg.g, attr->bg.b);
		for (i = 0; i < bb->cell_count; i++)
			damage_cell(bb, i);
	} else if (display_has_damage(rdr->disp)) {
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

struct render_ops render_bbulk_ops = {
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
};
