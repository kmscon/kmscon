/*
 * kmscon - Fixed 8x16 font
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
 * SECTION:font_8x16.c
 * @short_description: Fixed 8x16 font
 * @include: font.h
 *
 * This is a fixed font renderer backend that supports just one font which is
 * statically compiled into the file. This font is a very simple 8x16 font with
 * several special chars according to DEC-special-sets and common linux kernel
 * character-sets.
 *
 * When loading a font-face via this backend, then the static font is always
 * returned. This means, we need no internal state and can instead share the
 * buffer without locking. Every character outside of Latin1 is ignored so most
 * Unicode characters cannot be drawn with this backend.
 *
 * There is also no sophisticated font handling in here so this should only be
 * used as last fallback when external libraries failed. However, this also
 * means this backend is _very_ fast as no rendering is needed. Everything is
 * pre-rendered. See the big array at the end of this file for the bitmap.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "font_8x16.data.bin.h"
#include "shl/log.h"

#define LOG_SUBSYSTEM "font_8x16"

static int kmscon_font_8x16_init(struct kmscon_font *out, const struct kmscon_font_attr *attr)
{
	static const char name[] = "static-8x16";
	unsigned int scale;

	log_debug("loading static 8x16 font");

	memset(&out->attr, 0, sizeof(out->attr));
	memcpy(out->attr.name, name, sizeof(name));

	scale = (attr->height + 8) / 16;
	if (!scale)
		scale = 1;
	out->attr.bold = false;
	out->attr.italic = false;
	out->attr.width = 8 * scale;
	out->attr.height = 16 * scale;
	out->increase_step = 16;

	return 0;
}

static void kmscon_font_8x16_destroy(struct kmscon_font *font)
{
	log_debug("unloading static 8x16 font");
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

static struct kmscon_glyph *new_glyph(uint32_t ch, const struct kmscon_font_attr *attr,
				      unsigned int scale)
{
	const char *font_data;
	struct kmscon_glyph *glyph;
	unsigned int w = 8 * scale;
	unsigned int h = 16 * scale;
	uint8_t c;
	int i, j, k, l;

	font_data = &_binary_font_8x16_data_start[16 * ch];
	if (font_data + 16 > _binary_font_8x16_data_end)
		return NULL;

	glyph = malloc(sizeof(*glyph) + w * h);
	if (!glyph)
		return NULL;

	glyph->double_width = false;
	glyph->buf.width = w;
	glyph->buf.height = h;
	glyph->buf.stride = w;

	for (i = 0; i < h; i++) {
		for (j = 0; j < w; j++) {
			k = i / scale;
			l = j / scale;
			c = apply_attr((uint8_t)font_data[k], attr, k == 15);
			glyph->buf.data[i * glyph->buf.stride + j] = unfold(c & (1 << (7 - l)));
		}
	}
	return glyph;
}

static bool kmscon_font_8x16_has_glyph(struct kmscon_font *font, uint32_t ch)
{
	return (ch < 256);
}

static struct kmscon_glyph *kmscon_font_8x16_render(struct kmscon_font *font, uint32_t ch)
{
	unsigned int scale = font->attr.height / 16;

	if (ch == FONT_FULL_BLOCK)
		ch = 219;
	else if (ch == FONT_VBAR)
		ch = 179;
	if (ch >= 256)
		return new_glyph('?', &font->attr, scale);

	return new_glyph(ch, &font->attr, scale);
}

struct kmscon_font_ops kmscon_font_8x16_ops = {
	.name = "8x16",
	.owner = NULL,
	.init = kmscon_font_8x16_init,
	.destroy = kmscon_font_8x16_destroy,
	.has_glyph = kmscon_font_8x16_has_glyph,
	.render = kmscon_font_8x16_render,
};
