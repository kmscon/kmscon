/*
 * kmscon - PC Screen font backend
 *
 * Copyright (c) awsq.code <awsq.code@gmail.com>
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
 *
 * NOTICE:
 * Code may include fragments by David Herrman <dh.herrmann@googlemail.com>
 * licensed under MIT License
 */

/**
 * SECTION:font_psf.c
 * @short_description: PC Screen font
 * @include: font.h
 *
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "font.h"
#include "shl/log.h"

#define LOG_SUBSYSTEM "font_psf"

#define IS_GZ_MAGIC(m) (m[0] == 0x1f && m[1] == 0x8B)
#define IS_PSF1_MAGIC(m) (m[0] == 0x36 && m[1] == 0x04)
#define IS_PSF2_MAGIC(m) (m[0] == 0x72 && m[1] == 0xb5 && m[2] == 0x4a && m[3] == 0x86)

#define FREAD(f, to, size, err, ...)                                                               \
	if (_fread(f, to, size) < (size)) {                                                        \
		log_error(err);                                                                    \
		__VA_ARGS__                                                                        \
		goto err_file;                                                                     \
	}

typedef struct {
	uint32_t cap;
	uint16_t data[];
} *unicode_table_t;

typedef struct {
	uint32_t has_unicode_table;
	uint32_t glyphs;
	uint32_t step;
	uint32_t height;
	uint32_t width;
	uint32_t scale;
	unicode_table_t unicode_table;
	uint8_t data[];
} psf_font_t;

static int fread_(void *, void *, unsigned);
typedef int (*fseek_t)(void *, long int, int);
typedef int (*fread_t)(void *, void *, unsigned);
typedef int (*fclose_t)(void *);

static fseek_t _fseek;
static fread_t _fread;
static fclose_t _fclose;

static unicode_table_t unicode_table_parse(void *, uint8_t);
static uint16_t unicode_table_get(unicode_table_t, uint32_t);
static void unicode_table_exit(unicode_table_t);

static int kmscon_font_psf_init(struct kmscon_font *out, const struct kmscon_font_attr *attr)
{
	unsigned char magic[4];
	psf_font_t *font = NULL;
	uint32_t has_unicode_table, glyphs, step, height, width;

	_fseek = (fseek_t)fseek;
	_fread = (fread_t)fread_;
	_fclose = (fclose_t)fclose;

	void *font_file = fopen(attr->name, "rb");
	if (!font_file) {
		log_error("failed open psf font: %s", attr->name);
		return 1;
	}

	FREAD(font_file, &magic, 4, "failed read magic");

	if (IS_GZ_MAGIC(magic)) {
		_fseek(font_file, 0, SEEK_SET);
		font_file = gzdopen(fileno(font_file), "rb");
		if (!font_file) {
			log_error("failed open font as gz: %s", attr->name);
			return 1;
		}
		_fseek = (fseek_t)gzseek;
		_fread = (fread_t)gzread;
		_fclose = (fclose_t)gzclose;
		FREAD(font_file, &magic, 4, "failed read magic");
	}

	if (IS_PSF1_MAGIC(magic)) {
		glyphs = (magic[2] & 0x01) ? 512 : 256;
		has_unicode_table = magic[2] & 0b110;
		width = 8;
		height = magic[3];
		step = height;
	} else if (IS_PSF2_MAGIC(magic)) {
		_fseek(font_file, 12, SEEK_SET);
		FREAD(font_file, &has_unicode_table, 4, "failed read flags");
		FREAD(font_file, &glyphs, 4, "failed read glyphs");
		FREAD(font_file, &step, 4, "failed read step");
		FREAD(font_file, &height, 4, "failed read height");
		FREAD(font_file, &width, 4, "failed read width");
	} else {
		log_error("file isn't psf1 or psf2");
		goto err_file;
	}

	font = malloc(sizeof(*font) + step * glyphs);
	if (!font) {
		log_error("failed allocate font data");
		goto err_file;
	}
	font->has_unicode_table = has_unicode_table;
	font->glyphs = glyphs;
	font->step = step;
	font->height = height;
	font->width = width;

	FREAD(font_file, font->data, glyphs * step, "file is too short to store all font glyphs");

	if (has_unicode_table)
		if (!(font->unicode_table = unicode_table_parse(font_file, IS_PSF2_MAGIC(magic)))) {
			log_error("failed parse unicode table");
			goto err_file;
		}

	_fclose(font_file);

	memcpy(out->attr.name, attr->name, strlen(attr->name));

	font->scale = (attr->height + (height / 2)) / height;
	if (!font->scale)
		font->scale = 1;
	out->data = font;

	out->attr.bold = false;
	out->attr.italic = false;
	out->attr.width = width * font->scale;
	out->attr.height = height * font->scale;
	out->increase_step = height;

	log_notice("using font: %s %dx%d, scale %d, glyphs %d", attr->name, width, height,
		   font->scale, font->glyphs);

	return 0;

err_file:
	_fclose(font_file);
	return 1;
}

static void kmscon_font_psf_destroy(struct kmscon_font *kfont)
{
	psf_font_t *font = kfont->data;
	log_debug("unloading psf font");
	unicode_table_exit(font->unicode_table);
	free(font);
}

static uint32_t apply_attr(uint32_t c, const struct kmscon_font_attr *attr, bool last_line)
{
	if (attr->bold)
		c |= c >> 1;
	if (attr->underline && last_line)
		c = 0xffffffff;
	return c;
}

static uint8_t unfold(uint32_t val)
{
	return 0xff * !!val;
}

static uint32_t readrow(const uint8_t *data, uint8_t width)
{
	uint32_t row = 0;
	uint8_t len = (width + 7) / 8;

	for (uint8_t i = 0; i < len; i++)
		row = (row << 8) | data[i];

	return row >> (len * 8 - width);
}

static struct kmscon_glyph *new_glyph(uint32_t ch, const struct kmscon_font *kfont)
{
	struct kmscon_glyph *glyph;
	unsigned int w = kfont->attr.width;
	unsigned int h = kfont->attr.height;
	psf_font_t *font = kfont->data;
	uint8_t *glyph_data = font->data + ch * font->step;
	uint32_t c;
	int i, j, k, l;

	glyph = malloc(sizeof(*glyph) + w * h);
	if (!glyph) {
		log_error("failed allocate memory for glyph");
		return NULL;
	}

	glyph->double_width = false;
	glyph->buf.width = w;
	glyph->buf.height = h;

	for (i = 0; i < h; i++) {
		k = i / font->scale;
		c = apply_attr(readrow(glyph_data + k * (font->step / font->height), font->width),
			       &kfont->attr, k == (font->height - 1));

		for (j = 0; j < w; j++) {
			l = j / font->scale;
			glyph->buf.data[i * glyph->buf.width + j] =
				unfold(c & (1 << (font->width - 1 - l)));
		}
	}
	return glyph;
}

static bool kmscon_font_psf_has_glyph(struct kmscon_font *kfont, uint32_t ch)
{
	psf_font_t *font = kfont->data;

	if (font->has_unicode_table)
		ch = unicode_table_get(font->unicode_table, ch);

	return (ch == FONT_FULL_BLOCK || ch == FONT_VBAR || ch < font->glyphs);
}

static struct kmscon_glyph *kmscon_font_psf_render(struct kmscon_font *kfont, uint32_t ch)
{
	psf_font_t *font = kfont->data;

	if (ch == FONT_FULL_BLOCK)
		ch = 219;
	else if (ch == FONT_VBAR)
		ch = 179;

	if (font->has_unicode_table)
		ch = unicode_table_get(font->unicode_table, ch);

	if (ch >= font->glyphs)
		return new_glyph('?', kfont);

	return new_glyph(ch, kfont);
}

static unicode_table_t unicode_table_init()
{
	unicode_table_t ut = malloc(4 + 256 * sizeof(uint16_t));
	if (!ut)
		return NULL;
	ut->cap = 256;
	return ut;
}

static void unicode_table_add(unicode_table_t *self, uint32_t code, uint16_t ch)
{
	if (code >= (*self)->cap)
		*self = realloc(*self, ((*self)->cap = (4 + code + 128) * sizeof(uint16_t)));
	(*self)->data[code] = ch;
}

unicode_table_t unicode_table_parse(void *file, uint8_t psf)
{
#define UCODE_SIZE 1024
	uint8_t ucode[UCODE_SIZE];
	uint32_t ucode_idx = 0;
	uint32_t ucode_size;
	uint16_t ch = 0;
	uint32_t code;

	unicode_table_t unicode_table = unicode_table_init();
	if (!unicode_table) {
		log_error("failed allocate memory for unicode table");
		return 0;
	}

read:
	while ((ucode_size = _fread(file, &ucode, UCODE_SIZE))) {
		if (psf) { /* TODO psf2 */
		} else {
			while (ucode_idx < ucode_size) {
				switch ((code = *(uint16_t *)(ucode + ucode_idx))) {
				case 0xfffe:
				case 0xffff:
					ch++;
					break;
				default:
					unicode_table_add(&unicode_table,
							  *(uint16_t *)(ucode + ucode_idx), ch);
				}
				ucode_idx += 2;
			}
			ucode_idx = 0;
			goto read;
		}
	}
	return unicode_table;
}

static uint16_t unicode_table_get(unicode_table_t self, uint32_t code)
{
	return self->data[code];
}

static void unicode_table_exit(unicode_table_t self)
{
	free(self);
}

static int fread_(void *src, void *dst, unsigned size)
{
	return fread(dst, 1, size, src);
}

struct kmscon_font_ops kmscon_font_psf_ops = {
	.name = "psf",
	.owner = NULL,
	.init = kmscon_font_psf_init,
	.destroy = kmscon_font_psf_destroy,
	.has_glyph = kmscon_font_psf_has_glyph,
	.render = kmscon_font_psf_render,
};
