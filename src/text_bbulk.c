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

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "shl_hashtable.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "text.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "text_bbulk"

#define ID_DAMAGED 0xd41146edd41146ed
#define ID_OVERFLOW 0x0c34f10110c34f10

// Max horizontal distance of two damaged cells to be merged in a damage rectangle
#define DAMAGE_MERGE_LEN 3

struct bbcell {
	uint64_t id;
	struct tsm_screen_attr attr;
	bool overflow;
};

struct bbulk {
	struct uterm_video_blend_req *reqs;
	unsigned int req_len;
	unsigned int req_total_len;
	struct tsm_screen_attr attr;
	struct shl_hashtable *glyphs;
	struct bbcell *prev;
	unsigned int cells;
	unsigned int sw;
	unsigned int sh;
	bool *damages;
	struct uterm_video_rect *damage_rects;
	unsigned int damage_rect_len;
	uint8_t redraw;
	unsigned int off_x;
	unsigned int off_y;
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

static void free_glyph(void *data)
{
	struct kmscon_glyph *glyph = data;

	free(glyph);
}

static void damage_cell(struct bbulk *bb, unsigned int off)
{
	bb->prev[off].id = ID_DAMAGED;
	bb->damages[off] = true;
}

static void compute_border(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		bb->off_x = (bb->sw - txt->cols * FONT_WIDTH(txt)) / 2;
		bb->off_y = (bb->sh - txt->rows * FONT_HEIGHT(txt)) / 2;
	} else {
		bb->off_x = (bb->sw - txt->rows * FONT_HEIGHT(txt)) / 2;
		bb->off_y = (bb->sh - txt->cols * FONT_WIDTH(txt)) / 2;
	}
}

static int bbulk_set(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int max_damage_rects;
	int i;

	memset(bb, 0, sizeof(*bb));

	bb->sw = uterm_display_get_width(txt->disp);
	bb->sh = uterm_display_get_height(txt->disp);

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

	bb->cells = txt->max_cols * txt->max_rows;
	bb->req_total_len = bb->cells + 1; /* + 1 for the mouse pointer */
	max_damage_rects = SHL_DIV_ROUND_UP(txt->max_cols, DAMAGE_MERGE_LEN + 1) * txt->max_rows;

	bb->reqs = malloc(sizeof(*bb->reqs) * bb->req_total_len);
	if (!bb->reqs)
		return -ENOMEM;
	memset(bb->reqs, 0, sizeof(*bb->reqs) * bb->req_total_len);

	bb->prev = malloc(sizeof(*bb->prev) * bb->cells);
	if (!bb->prev)
		goto free_reqs;
	memset(bb->prev, 0, sizeof(*bb->prev) * bb->cells);

	bb->damages = malloc(sizeof(*bb->damages) * bb->cells);
	if (!bb->damages)
		goto free_prev;

	bb->damage_rects = malloc(sizeof(*bb->damage_rects) * max_damage_rects);
	if (!bb->damage_rects)
		goto free_damages;

	for (i = 0; i < (int)bb->cells; i++)
		damage_cell(bb, i);

	if (shl_hashtable_new(&bb->glyphs, shl_direct_hash, shl_direct_equal, free_glyph))
		goto free_r_damages;
	return 0;

free_r_damages:
	free(bb->damage_rects);
free_damages:
	free(bb->damages);
free_prev:
	free(bb->prev);
free_reqs:
	free(bb->reqs);
	return -ENOMEM;
}

static void bbulk_unset(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	shl_hashtable_free(bb->glyphs);
	free(bb->damage_rects);
	free(bb->reqs);
	free(bb->damages);
	free(bb->prev);
	bb->glyphs = NULL;
	bb->damage_rects = NULL;
	bb->reqs = NULL;
	bb->damages = NULL;
	bb->prev = NULL;
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
	struct uterm_video_buffer *buf = &glyph->buf;
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

static struct kmscon_glyph *find_glyph(struct kmscon_text *txt, uint64_t id, const uint32_t *ch,
				       size_t len, const struct tsm_screen_attr *attr)
{
	struct bbulk *bb = txt->data;
	struct kmscon_glyph *glyph;
	struct kmscon_font *font = txt->font;

	font->attr.underline = !!attr->underline;
	font->attr.italic = !!attr->italic;
	font->attr.bold = !!attr->bold;

	if (shl_hashtable_find(bb->glyphs, (void **)&glyph, id))
		return glyph;

	glyph = kmscon_font_render(font, id, ch, len);
	if (!glyph)
		return NULL;

	if (txt->orientation != OR_NORMAL)
		glyph = bbulk_rotate_glyph(glyph, txt->orientation);

	if (shl_hashtable_insert(bb->glyphs, id, glyph)) {
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
		*x = posx * FONT_WIDTH(txt);
		*y = posy * FONT_HEIGHT(txt);
		break;
	case OR_UPSIDE_DOWN:
		*x = bb->sw - (posx + 1) * FONT_WIDTH(txt);
		*y = bb->sh - (posy + 1) * FONT_HEIGHT(txt);
		break;
	case OR_RIGHT:
		*x = bb->sw - (posy + 1) * FONT_HEIGHT(txt);
		*y = posx * FONT_WIDTH(txt);
		break;
	case OR_LEFT:
		*x = posy * FONT_HEIGHT(txt);
		*y = bb->sh - (posx + 1) * FONT_WIDTH(txt);
		break;
	}
	*x += bb->off_x;
	*y += bb->off_y;
}

static void set_color(struct uterm_video_blend_req *req, const struct tsm_screen_attr *attr)
{
	if (attr->inverse) {
		req->fr = attr->br;
		req->fg = attr->bg;
		req->fb = attr->bb;
		req->br = attr->fr;
		req->bg = attr->fg;
		req->bb = attr->fb;
	} else {
		req->fr = attr->fr;
		req->fg = attr->fg;
		req->fb = attr->fb;
		req->br = attr->br;
		req->bg = attr->bg;
		req->bb = attr->bb;
	}
}

static int bbulk_draw(struct kmscon_text *txt, uint64_t id, const uint32_t *ch, size_t len,
		      unsigned int width, unsigned int posx, unsigned int posy,
		      const struct tsm_screen_attr *attr)
{
	struct bbulk *bb = txt->data;
	struct kmscon_glyph *glyph;
	struct uterm_video_blend_req *req;
	struct bbcell *prev;
	unsigned int offset = posx + posy * txt->cols;
	bool last_col = (posx == txt->cols - 1);

	if (!width)
		return 0;

	if (!len && posx && bb->prev[offset - 1].overflow)
		return 0;

	prev = &bb->prev[offset];

	if (prev->id == id && !memcmp(&prev->attr, attr, sizeof(*attr))) {
		if ((prev->overflow || width == 2) && !last_col) {
			if (bb->damages[offset] || bb->damages[offset + 1] ||
			    bb->prev[offset + 1].id == ID_DAMAGED) {
				bb->damages[offset] = false;
				if (bb->prev[offset + 1].id == ID_OVERFLOW)
					bb->damages[offset + 1] = false;
				bb->prev[offset + 1].id = ID_OVERFLOW;
			} else {
				return 0;
			}
		} else {
			if (!bb->damages[offset])
				return 0;
			bb->damages[offset] = false;
		}
	} else {
		bb->damages[offset] = true;
		if ((prev->overflow || width == 2) && !last_col)
			damage_cell(bb, offset + 1);
	}

	prev->id = id;
	memcpy(&prev->attr, attr, sizeof(*attr));

	glyph = find_glyph(txt, id, ch, len, attr);
	if (!glyph)
		return -ENOMEM;

	if (glyph->double_width && !last_col) {
		prev->overflow = true;
		bb->prev[offset + 1].overflow = false;
	} else
		prev->overflow = false;

	req = &bb->reqs[bb->req_len++];

	if (prev->overflow && (txt->orientation == OR_LEFT || txt->orientation == OR_UPSIDE_DOWN))
		/*
		 * In case of left or upside down orientation, we need to draw to the
		 * next cell, as the glyph is already rotated, so start on the next cell
		 * and end on this cell
		 */
		set_coordinate(txt, &req->x, &req->y, posx + 1, posy);
	else
		set_coordinate(txt, &req->x, &req->y, posx, posy);

	req->buf = &glyph->buf;
	set_color(req, attr);

	if (width == 2 && !glyph->double_width && !last_col) {
		/* libtsm thinks this glyph is wide, but the font uses a single
		 * width character. So draw a space on next cell to avoid a
		 * graphical glitch
		 */
		glyph = find_glyph(txt, ' ', NULL, 0, attr);
		if (!glyph)
			return -ENOMEM;

		req = &bb->reqs[bb->req_len++];
		set_coordinate(txt, &req->x, &req->y, posx + 1, posy);
		req->buf = &glyph->buf;
		set_color(req, attr);
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

	damage_cell(bb, posx + posy * txt->cols);

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
				   struct uterm_video_blend_req *req, unsigned int pointer_x,
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
	struct uterm_video_blend_req *req;
	struct kmscon_glyph *bb_glyph;
	uint32_t ch = 'I';
	uint64_t id = ch;

	if (bb->req_len >= bb->req_total_len)
		return -ENOMEM;

	pointer_x = min(pointer_x, txt->cols * FONT_WIDTH(txt) - (FONT_WIDTH(txt) / 2));
	pointer_y = min(pointer_y, txt->rows * FONT_HEIGHT(txt) - (FONT_HEIGHT(txt) / 2));

	req = &bb->reqs[bb->req_len++];
	mark_damaged(txt, bb, pointer_x, pointer_y);

	bb_glyph = find_glyph(txt, id, &ch, 1, &bb->attr);
	if (!bb_glyph)
		return -ENOMEM;

	req->buf = &bb_glyph->buf;
	set_pointer_coordinate(bb, txt, req, pointer_x, pointer_y);

	req->fr = bb->attr.fr;
	req->fg = bb->attr.fg;
	req->fb = bb->attr.fb;
	req->br = bb->attr.br;
	req->bg = bb->attr.bg;
	req->bb = bb->attr.bb;
	return 0;
}

static void add_damage(struct bbulk *bb, struct uterm_video_rect *r)
{
	struct uterm_video_rect *out = &bb->damage_rects[bb->damage_rect_len];

	*out = *r;
	bb->damage_rect_len++;
}

static void merge_damage(struct bbulk *bb, struct uterm_video_rect *r)
{
	struct uterm_video_rect *out = &bb->damage_rects[bb->damage_rect_len - 1];

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
	struct uterm_video_rect r;
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
			if (!bb->damages[off]) {
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
	int ret;

	ret = uterm_display_fake_blendv(txt->disp, bb->reqs, bb->req_len);
	// log_debug("bbulk, redraw %d cells", bb->req_len);
	if (uterm_display_supports_damage(txt->disp)) {
		bbulk_compute_damage(txt);
		uterm_display_set_damage(txt->disp, bb->damage_rect_len, bb->damage_rects);
	}
	return ret;
}

static int bbulk_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr)
{
	struct bbulk *bb = txt->data;
	int i;

	// Clear previous requests
	for (i = 0; i < bb->req_total_len; ++i)
		bb->reqs[i].buf = NULL;

	bb->req_len = 0;
	bb->damage_rect_len = 0;

	/*
	 * if default colors have changed, or we switch from a dirty screen,
	 * redraw completely the next 2 frames.
	 */
	if (memcmp(&bb->attr, attr, sizeof(*attr)) || uterm_display_need_redraw(txt->disp))
		bb->redraw = 2;

	bb->attr = *attr;

	if (bb->redraw) {
		uterm_display_clear(txt->disp, attr->br, attr->bg, attr->bb);
		for (i = 0; i < bb->cells; i++)
			damage_cell(bb, i);
	} else if (uterm_display_has_damage(txt->disp)) {
		log_debug("Carry over damage from previous frame");
		for (i = 0; i < bb->cells; i++) {
			if (bb->damages[i])
				bb->prev[i].id = ID_DAMAGED;
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
