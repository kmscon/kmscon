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

#include <errno.h>
#include <libtsm.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "font_unifont_data.bin.h"
#include "shl_hashtable.h"
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
	uint8_t width;	    // glyph width (1 or 2 for double-width glyph)
} __attribute__((__packed__));

/*
 * Global glyph cache
 * The linked binary glyph data cannot be directly passed to the caller as it
 * has the wrong format. Hence, use a glyph-cache with all used glyphs and add
 * new ones as soon as they are used.
 */

static void free_glyph(void *data)
{
	struct kmscon_glyph *g = data;

	free(g->buf.data);
	free(g);
}

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

	while (min != max) {
		log_debug("lookup %d codep %d, look %d min %d max %d", ch, blocks[look].codepoint,
			  look, min, max);

		if (is_in_block(&blocks[look], ch))
			return look;

		if (ch < blocks[look].codepoint) {
			max = look;
			look -= look - min > 2 ? (look - min) / 2 : 1;
		} else {
			min = look;
			look += max - look > 2 ? (max - look) / 2 : 1;
		}
	}
	if (is_in_block(&blocks[look], ch))
		return look;
	return -1;
}

static struct kmscon_glyph *new_glyph(const struct kmscon_font_attr *attr, const uint8_t *data,
				      int width)
{
	struct kmscon_glyph *g;
	uint8_t c;
	int scale;
	int i, j, k;
	int off = 0;

	scale = attr->height / 16;
	g = malloc(sizeof(*g));
	if (!g)
		return NULL;
	memset(g, 0, sizeof(*g));
	g->width = width;
	g->buf.width = g->width * attr->width;
	g->buf.height = attr->height;
	g->buf.stride = g->width * attr->width;
	g->buf.format = UTERM_FORMAT_GREY;

	g->buf.data = malloc(g->buf.stride * g->buf.height);
	if (!g->buf.data) {
		free(g);
		return NULL;
	}

	/* Unpack the glyph and apply scaling */
	for (i = 0; i < 16; i++) {
		c = apply_attr(data[g->width * i], attr, i == 15);
		for (j = 0; j < g->buf.width / g->width; j++) {
			k = j / scale;
			g->buf.data[off++] = unfold(c & (1 << (7 - k)));
		}
		if (g->width == 2) {
			c = apply_attr(data[g->width * i + 1], attr, i == 15);
			for (j = 0; j < g->buf.width / g->width; j++) {
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

static int find_glyph(uint64_t id, const struct kmscon_glyph **out, const struct kmscon_font *font)
{
	struct shl_hashtable *cache = font->data;
	struct kmscon_glyph *g;
	uint32_t ch = id & TSM_UCS4_MAX;
	const void *start = _binary_font_unifont_data_start;
	const uint8_t *end = (uint8_t *)_binary_font_unifont_data_end;
	const uint8_t *data;
	uint32_t len;
	const struct unifont_glyph_block *blocks;
	int ret;
	bool res;

	res = shl_hashtable_find(cache, (void **)out, id);
	if (res)
		return 0;

	/* First the length of the block index */
	len = *((uint32_t *)start);
	/* Then the block index */
	blocks = (struct unifont_glyph_block *)(start + 4);
	/* Then the glyph data */
	data = (uint8_t *)start + 4 + len * sizeof(struct unifont_glyph_block);

	int idx = lookup_block(blocks, len, ch);
	if (idx < 0) {
		log_debug("codepoint %08x not found, using replacement glyph", ch);
		ch = 0xfffd;
		idx = lookup_block(blocks, len, ch);
		if (idx < 0) {
			log_warning("Replacement glyph not found");
			return -EINVAL;
		}
	}

	data += blocks[idx].offset + (ch - blocks[idx].codepoint) * blocks[idx].width * 16;
	if (data + 16 * blocks[idx].width > end) {
		log_warning("glyph out of range %p %p", data, end);
		return -ERANGE;
	}

	g = new_glyph(&font->attr, data, blocks[idx].width);
	if (!g)
		return -ENOMEM;

	ret = shl_hashtable_insert(cache, id, g);
	if (ret) {
		log_error("cannot insert glyph into glyph-cache: %d", ret);
		goto err_data;
	}

	*out = g;
	return 0;

err_data:
	free(g->buf.data);
	free(g);
	return ret;
}

static int kmscon_font_unifont_init(struct kmscon_font *out, const struct kmscon_font_attr *attr)
{
	static const char name[] = "static-unifont";
	struct shl_hashtable *cache;
	unsigned int scale;
	int ret;

	log_debug("loading static unifont font");

	ret = shl_hashtable_new(&cache, shl_direct_hash, shl_direct_equal, free_glyph);
	if (ret)
		return ret;

	if (_binary_font_unifont_data_size == 0) {
		log_error("unifont glyph information not found in binary");
		goto err_free;
	}

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
	out->data = cache;

	return 0;

err_free:
	shl_hashtable_free(cache);
	return -EFAULT;
}

static void kmscon_font_unifont_destroy(struct kmscon_font *font)
{
	struct shl_hashtable *cache = font->data;

	log_debug("unloading static unifont font");

	shl_hashtable_free(cache);
}

static int kmscon_font_unifont_render(struct kmscon_font *font, uint64_t id, const uint32_t *ch,
				      size_t len, const struct kmscon_glyph **out)
{
	if (len > 1)
		return -ERANGE;

	return find_glyph(id, out, font);
}

static int kmscon_font_unifont_render_inval(struct kmscon_font *font,
					    const struct kmscon_glyph **out)
{
	return find_glyph(0xfffd, out, font);
}

static int kmscon_font_unifont_render_empty(struct kmscon_font *font,
					    const struct kmscon_glyph **out)
{
	return find_glyph(' ', out, font);
}

struct kmscon_font_ops kmscon_font_unifont_ops = {
	.name = "unifont",
	.owner = NULL,
	.init = kmscon_font_unifont_init,
	.destroy = kmscon_font_unifont_destroy,
	.render = kmscon_font_unifont_render,
	.render_empty = kmscon_font_unifont_render_empty,
	.render_inval = kmscon_font_unifont_render_inval,
};
