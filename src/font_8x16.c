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
#include "shl_log.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "font_8x16"

static int kmscon_font_8x16_init(struct kmscon_font *out, const struct kmscon_font_attr *attr)
{
	static const char name[] = "static-8x16";

	log_debug("loading static 8x16 font");

	memset(&out->attr, 0, sizeof(out->attr));
	memcpy(out->attr.name, name, sizeof(name));
	out->attr.bold = false;
	out->attr.italic = false;
	out->attr.width = 8;
	out->attr.height = 16;
	kmscon_font_attr_normalize(&out->attr);

	return 0;
}

static void kmscon_font_8x16_destroy(struct kmscon_font *font)
{
	log_debug("unloading static 8x16 font");
}

static uint8_t unfold(uint8_t val)
{
	return 0xff * !!val;
}

static struct kmscon_glyph *new_glyph(uint32_t ch)
{
	const char *font_data;
	struct kmscon_glyph *glyph;
	uint8_t c;
	int i, j;

	font_data = &_binary_font_8x16_data_start[16 * ch];
	if (font_data + 16 > _binary_font_8x16_data_end)
		return NULL;

	glyph = malloc(sizeof(*glyph) + 8 * 16);
	if (!glyph)
		return NULL;

	glyph->width = 1;
	glyph->buf.width = 8;
	glyph->buf.height = 16;
	glyph->buf.stride = 8;
	glyph->buf.format = UTERM_FORMAT_GREY;

	for (i = 0; i < 16; i++) {
		for (j = 0; j < 8; j++) {
			c = (uint8_t)font_data[i];
			glyph->buf.data[i * 8 + j] = unfold(c & (1 << (7 - j)));
		}
	}
	return glyph;
}

static int kmscon_font_8x16_render(struct kmscon_font *font, uint64_t id, const uint32_t *ch,
				   size_t len, const struct kmscon_glyph **out)
{
	struct kmscon_glyph *glyph;

	if (len > 1 || *ch >= 256)
		return -ERANGE;

	glyph = new_glyph(*ch);
	if (!glyph)
		return -ENOMEM;

	*out = glyph;
	return 0;
}

static int kmscon_font_8x16_render_empty(struct kmscon_font *font, const struct kmscon_glyph **out)
{
	*out = new_glyph(' ');
	return 0;
}

static int kmscon_font_8x16_render_inval(struct kmscon_font *font, const struct kmscon_glyph **out)
{
	*out = new_glyph('?');
	return 0;
}

struct kmscon_font_ops kmscon_font_8x16_ops = {
	.name = "8x16",
	.owner = NULL,
	.init = kmscon_font_8x16_init,
	.destroy = kmscon_font_8x16_destroy,
	.render = kmscon_font_8x16_render,
	.render_empty = kmscon_font_8x16_render_empty,
	.render_inval = kmscon_font_8x16_render_inval,
};
