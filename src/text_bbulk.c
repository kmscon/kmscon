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
#include "font_rotate.h"
#include "shl_hashtable.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "text.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "text_bbulk"

#define ID_DAMAGED 0xd41146edd41146ed
#define ID_OVERFLOW 0x0c34f10110c34f10

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
	struct shl_hashtable *bold_glyphs;
	struct bbcell *prev;
	unsigned int cells;
	unsigned int sw;
	unsigned int sh;
	bool *damages;
};

static int bbulk_init(struct kmscon_text *txt)
{
	struct bbulk *bb;

	bb = malloc(sizeof(*bb));
	if (!bb)
		return -ENOMEM;

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
	struct uterm_video_buffer *bb_glyph = data;

	free(bb_glyph->data);
	free(bb_glyph);
}

static int bbulk_set(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int i;

	memset(bb, 0, sizeof(*bb));

	bb->sw = uterm_display_get_width(txt->disp);
	bb->sh = uterm_display_get_height(txt->disp);

	if (!bb->sw || !bb->sh)
		return -EINVAL;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		txt->cols = bb->sw / FONT_WIDTH(txt);
		txt->rows = bb->sh / FONT_HEIGHT(txt);
	} else {
		txt->rows = bb->sw / FONT_HEIGHT(txt);
		txt->cols = bb->sh / FONT_WIDTH(txt);
	}
	bb->cells = txt->cols * txt->rows;

	bb->req_total_len = bb->cells + 1; // + 1 for the mouse pointer
	bb->reqs = malloc(sizeof(*bb->reqs) * bb->req_total_len);
	if (!bb->reqs)
		return -ENOMEM;
	memset(bb->reqs, 0, sizeof(*bb->reqs) * bb->req_total_len);

	bb->prev = malloc(sizeof(*bb->prev) * bb->cells);
	if (!bb->prev)
		goto free_reqs;

	bb->damages = malloc(sizeof(*bb->damages) * bb->cells);
	if (!bb->damages)
		goto free_prev;

	for (i = 0; i < bb->cells; i++) {
		// start with all cells damaged
		bb->prev[i].id = ID_DAMAGED;
		bb->damages[i] = true;
	}

	if (kmscon_rotate_create_tables(&bb->glyphs, &bb->bold_glyphs, free_glyph))
		goto free_damages;
	return 0;

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

	kmscon_rotate_free_tables(bb->glyphs, bb->bold_glyphs);
	free(bb->reqs);
	free(bb->damages);
	free(bb->prev);
	bb->reqs = NULL;
	bb->damages = NULL;
	bb->prev = NULL;
}

static int bbulk_rotate(struct kmscon_text *txt, enum Orientation orientation)
{
	bbulk_unset(txt);
	txt->orientation = orientation;
	return bbulk_set(txt);
}

static int find_glyph(struct kmscon_text *txt, struct kmscon_glyph **out, uint64_t id,
		      const uint32_t *ch, size_t len, const struct tsm_screen_attr *attr)
{
	struct bbulk *bb = txt->data;
	struct kmscon_glyph *bb_glyph;
	const struct kmscon_glyph *glyph;
	struct shl_hashtable *gtable;
	struct kmscon_font *font;
	int ret;
	bool res;

	if (attr->bold) {
		gtable = bb->bold_glyphs;
		font = txt->bold_font;
	} else {
		gtable = bb->glyphs;
		font = txt->font;
	}

	if (attr->underline)
		font->attr.underline = true;
	else
		font->attr.underline = false;

	if (attr->italic)
		font->attr.italic = true;
	else
		font->attr.italic = false;

	res = shl_hashtable_find(gtable, (void **)&bb_glyph, id);
	if (res) {
		*out = bb_glyph;
		return 0;
	}

	bb_glyph = malloc(sizeof(*bb_glyph));
	if (!bb_glyph)
		return -ENOMEM;
	memset(bb_glyph, 0, sizeof(*bb_glyph));

	if (!len)
		ret = kmscon_font_render_empty(font, &glyph);
	else
		ret = kmscon_font_render(font, id, ch, len, &glyph);

	if (ret) {
		ret = kmscon_font_render_inval(font, &glyph);
		if (ret)
			goto err_free;
	}

	ret = kmscon_rotate_glyph(bb_glyph, glyph, txt->orientation, 1);
	if (ret)
		goto err_free;

	ret = shl_hashtable_insert(gtable, id, bb_glyph);
	if (ret)
		goto err_free_vb;

	*out = bb_glyph;
	return 0;

err_free_vb:
	free(bb_glyph->buf.data);
err_free:
	free(bb_glyph);
	return ret;
}

static void set_coordinate(struct kmscon_text *txt, struct uterm_video_blend_req *req,
			   unsigned int sw, unsigned int sh, unsigned int posx, unsigned int posy)
{
	switch (txt->orientation) {
	case OR_NORMAL:
		req->x = posx * FONT_WIDTH(txt);
		req->y = posy * FONT_HEIGHT(txt);
		break;
	case OR_UPSIDE_DOWN:
		req->x = sw - (posx + 1) * FONT_WIDTH(txt);
		req->y = sh - (posy + 1) * FONT_HEIGHT(txt);
		break;
	case OR_RIGHT:
		req->x = sw - (posy + 1) * FONT_HEIGHT(txt);
		req->y = posx * FONT_WIDTH(txt);
		break;
	case OR_LEFT:
		req->x = posy * FONT_HEIGHT(txt);
		req->y = sh - (posx + 1) * FONT_WIDTH(txt);
		break;
	}
}

static int bbulk_draw(struct kmscon_text *txt, uint64_t id, const uint32_t *ch, size_t len,
		      unsigned int width, unsigned int posx, unsigned int posy,
		      const struct tsm_screen_attr *attr)
{
	struct bbulk *bb = txt->data;
	struct kmscon_glyph *bb_glyph;
	struct uterm_video_blend_req *req;
	struct bbcell *prev;
	unsigned int offset = posx + posy * txt->cols;
	int ret;

	if (!width)
		return 0;

	if (!len && posx && bb->prev[offset - 1].overflow)
		return 0;

	prev = &bb->prev[offset];

	if (prev->id == id && !memcmp(&prev->attr, attr, sizeof(*attr))) {
		if (prev->overflow) {
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
			if (!bb->damages[offset]) {
				return 0;
			} else {
				bb->damages[offset] = false;
			}
		}
	} else {
		bb->damages[offset] = true;
	}

	prev->id = id;
	memcpy(&prev->attr, attr, sizeof(*attr));

	ret = find_glyph(txt, &bb_glyph, id, ch, len, attr);
	if (ret)
		return ret;

	if (bb_glyph->width == 2 && posx + 1 < txt->cols)
		prev->overflow = true;
	else
		prev->overflow = false;

	req = &bb->reqs[bb->req_len++];

	if (prev->overflow && (txt->orientation == OR_LEFT || txt->orientation == OR_UPSIDE_DOWN))
		/*
		 * In case of left or upside down orientation, we need to draw to the
		 * next cell, as the glyph is already rotated, so start on the next cell
		 * and end on this cell
		 */
		set_coordinate(txt, req, bb->sw, bb->sh, posx + 1, posy);
	else
		set_coordinate(txt, req, bb->sw, bb->sh, posx, posy);

	req->buf = &bb_glyph->buf;
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
	return 0;
}

/*
 * When the pointer move over, mark the 4 underlying cells as damaged.
 */
static void mark_damaged(struct kmscon_text *txt, struct bbulk *bb, unsigned int x, unsigned int y)
{
	unsigned int posx = 0;
	unsigned int posy = 0;
	unsigned int fw, fh;
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

	bb->damages[posx + txt->cols * posy] = true;
	bb->prev[posx + txt->cols * posy].id = ID_DAMAGED;

	if (posx + 1 < txt->cols) {
		bb->damages[posx + 1 + txt->cols * posy] = true;
		bb->prev[posx + 1 + txt->cols * posy].id = ID_DAMAGED;
	}
	if (posy + 1 < txt->rows) {
		bb->damages[posx + txt->cols * (posy + 1)] = true;
		bb->prev[posx + txt->cols * (posy + 1)].id = ID_DAMAGED;
	}
	if (posx + 1 < txt->cols && posy + 1 < txt->rows) {
		bb->damages[posx + 1 + txt->cols * (posy + 1)] = true;
		bb->prev[posx + 1 + txt->cols * (posy + 1)].id = ID_DAMAGED;
	}
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

	x -= hf_w;
	y -= hf_h;

	req->x = x;
	req->y = y;
}

static int bblit_draw_pointer(struct kmscon_text *txt, unsigned int pointer_x,
			      unsigned int pointer_y)
{
	struct bbulk *bb = txt->data;
	struct uterm_video_blend_req *req;
	struct kmscon_glyph *bb_glyph;
	uint32_t ch = 'I';
	uint64_t id = ch;

	int ret;

	if (bb->req_len >= bb->req_total_len)
		return -ENOMEM;

	req = &bb->reqs[bb->req_len++];
	mark_damaged(txt, bb, pointer_x, pointer_y);

	ret = find_glyph(txt, &bb_glyph, id, &ch, 1, &bb->attr);
	if (ret)
		return ret;

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

static int bbulk_render(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	int ret;

	ret = uterm_display_fake_blendv(txt->disp, bb->reqs, bb->req_len);
	log_debug("bbulk, redraw %d cells", bb->req_len);
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
	bb->attr = *attr;

	return 0;
}

struct kmscon_text_ops kmscon_text_bbulk_ops = {
	.name = "bbulk",
	.owner = NULL,
	.init = bbulk_init,
	.destroy = bbulk_destroy,
	.set = bbulk_set,
	.unset = bbulk_unset,
	.rotate = bbulk_rotate,
	.prepare = bbulk_prepare,
	.draw = bbulk_draw,
	.draw_pointer = bblit_draw_pointer,
	.render = bbulk_render,
	.abort = NULL,
};
