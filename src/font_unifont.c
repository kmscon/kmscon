/*
 * kmscon - Fixed unifont font
 *
 * Copyright (c) 2012 Ted Kotz <ted@kotz.us>
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
 * SECTION:font_unifont.c
 * @short_description: Fixed unifont font
 * @include: font.h
 *
 * This is a fixed font renderer backend that supports just one font which is
 * statically compiled into the file. This bitmap font has 8x16 and 16x16
 * glyphs. This can statically compile in any font defined as a unifont style
 * hex format. This font is from the GNU unifont project available at:
 *   http://unifoundry.com/unifont.html
 *
 * This file is heavily based on font_8x16.c
 */

#include <libtsm.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "font.h"
#include "font_unifont_data.bin.h"
#include "shl_log.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "font_unifont"

/*
 * We regroup all glyphs into blocks, of contiguous codepoints, and same width.
 * This allows to better pack the data, and handle some codepoints that are
 * not in the 0xffff range
 */
struct unifont_glyph_block {
	uint32_t codepoint; // First codepoint of the block
	uint32_t offset;    // offset of the data
	uint16_t len;	    // number of glyph in this block
	uint8_t cwidth;	    // glyph width (1 or 2 for double-width glyph)
} __attribute__((__packed__));

struct unifont_data {
	unsigned char *font_data;
	unsigned long len;
};

static uint8_t apply_attr(uint8_t c, const struct kmscon_font_attr *attr, bool last_line)
{
	if (attr->bold)
		c |= c >> 1;
	if (attr->underline && last_line)
		c = 0xff;
	return c;
}

static uint8_t unfold(uint8_t val)
{
	return 0xff * !!val;
}

static bool is_in_block(const struct unifont_glyph_block *idx, uint32_t ch)
{
	return (ch >= idx->codepoint && ch < idx->codepoint + idx->len);
}

static int lookup_block(const struct unifont_glyph_block *blocks, uint32_t len, uint32_t ch)
{
	int look = 1 + ((ch * len) / 0xffff); /* opportunist first look*/
	int min = 0;
	int max = len - 1;

	if (look > max)
		look = max;

	while (min < max) {
		if (is_in_block(&blocks[look], ch))
			return look;

		if (ch < blocks[look].codepoint) {
			max = look - 1;
			look = min + (max - min) / 2;
		} else {
			min = look + 1;
			look = max - (max - min) / 2;
		}
	}
	if (is_in_block(&blocks[look], ch))
		return look;
	return -1;
}

static struct kmscon_glyph *new_glyph(const struct kmscon_font_attr *attr, const uint8_t *data,
				      int cwidth)
{
	struct kmscon_glyph *g;
	uint8_t c;
	int scale;
	int i, j, k;
	int off = 0;

	scale = attr->height / 16;
	g = malloc(sizeof(*g) + cwidth * attr->width * attr->height);
	if (!g)
		return NULL;
	memset(g, 0, sizeof(*g));
	g->double_width = (cwidth == 2);
	g->buf.width = cwidth * attr->width;
	g->buf.height = attr->height;
	g->buf.stride = g->buf.width;

	/* Unpack the glyph and apply scaling */
	for (i = 0; i < 16; i++) {
		c = apply_attr(data[cwidth * i], attr, i == 15);
		for (j = 0; j < g->buf.width / cwidth; j++) {
			k = j / scale;
			g->buf.data[off++] = unfold(c & (1 << (7 - k)));
		}
		if (g->double_width) {
			c = apply_attr(data[cwidth * i + 1], attr, i == 15);
			for (j = 0; j < g->buf.width / cwidth; j++) {
				k = j / scale;
				g->buf.data[off++] = unfold(c & (1 << (7 - k)));
			}
		}
		for (k = 1; k < scale; k++) {
			memcpy(&g->buf.data[off], &g->buf.data[i * scale * g->buf.stride],
			       g->buf.stride);
			off += g->buf.stride;
		}
	}
	return g;
}

static struct kmscon_glyph *find_glyph(uint64_t id, const struct kmscon_font *font)
{
	struct unifont_data *uf = font->data;
	uint32_t ch = id & TSM_UCS4_MAX;
	const uint8_t *data;
	uint32_t len;
	const struct unifont_glyph_block *blocks;

	/* First the length of the block index */
	len = *((uint32_t *)uf->font_data);
	/* Then the block index */
	blocks = (struct unifont_glyph_block *)(uf->font_data + 4);
	/* Then the glyph data */
	data = (uint8_t *)uf->font_data + 4 + len * sizeof(struct unifont_glyph_block);

	int idx = lookup_block(blocks, len, ch);
	if (idx < 0)
		return NULL;

	data += blocks[idx].offset + (ch - blocks[idx].codepoint) * blocks[idx].cwidth * 16;
	if (data + 16 * blocks[idx].cwidth > uf->font_data + uf->len) {
		log_warning("glyph out of range %p %p", data, uf->font_data + uf->len);
		return NULL;
	}

	return new_glyph(&font->attr, data, blocks[idx].cwidth);
}

static int kmscon_font_unifont_init(struct kmscon_font *out, const struct kmscon_font_attr *attr)
{
	static const char name[] = "static-unifont";
	struct unifont_data *uf;
	unsigned int scale;

	log_debug("loading static unifont font");
	if (_binary_font_unifont_data_size == 0) {
		log_error("unifont glyph information not found in binary");
		return -EINVAL;
	}

	uf = malloc(sizeof(*uf));
	if (!uf)
		return -ENOMEM;

	uf->len = *((uint32_t *)_binary_font_unifont_data_start);
	uf->font_data = malloc(uf->len);
	if (!uf->font_data)
		goto err_free;

	if (uncompress(uf->font_data, &uf->len,
		       (unsigned char *)_binary_font_unifont_data_start + 4,
		       _binary_font_unifont_data_size - 4) != Z_OK)
		goto err_free_data;

	memset(&out->attr, 0, sizeof(out->attr));
	memcpy(out->attr.name, name, sizeof(name));
	out->attr.bold = attr->bold;
	out->attr.italic = false;

	scale = (attr->points + 8) / 16;
	if (!scale)
		scale = 1;

	out->attr.width = 8 * scale;
	out->attr.height = 16 * scale;
	kmscon_font_attr_normalize(&out->attr);
	out->increase_step = 16;
	out->data = uf;

	return 0;

err_free_data:
	free(uf->font_data);
err_free:
	free(uf);
	return -EFAULT;
}

static void kmscon_font_unifont_destroy(struct kmscon_font *font)
{
	struct unifont_data *uf = font->data;

	log_debug("unloading static unifont font");
	free(uf->font_data);
	free(uf);
}

static struct kmscon_glyph *kmscon_font_unifont_render(struct kmscon_font *font, uint64_t id,
						       const uint32_t *ch, size_t len)
{
	if (len > 1)
		return NULL;

	return find_glyph(id, font);
}

struct kmscon_font_ops kmscon_font_unifont_ops = {
	.name = "unifont",
	.owner = NULL,
	.init = kmscon_font_unifont_init,
	.destroy = kmscon_font_unifont_destroy,
	.render = kmscon_font_unifont_render,
};
