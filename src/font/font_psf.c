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

#define PSF1_FLAG_512 0x1
#define PSF1_FLAG_TAB 0x2
#define PSF1_FLAG_SEQ 0x4

#define PSF2_FLAG_TAB 0x1

#define FREAD(f, to, size, err, ...)                                                               \
	if (_fread(f, to, size) < (size)) {                                                        \
		log_error(err);                                                                    \
		__VA_ARGS__                                                                        \
		goto err_file;                                                                     \
	}

typedef struct {
	uint16_t glyph;
	uint32_t code;
} unicode_entry_t;

typedef struct {
	uint32_t cap;
	uint32_t size;
	unicode_entry_t *data;
} unicode_table_t;

typedef struct {
	uint32_t glyphs;
	uint32_t step;
	uint32_t height;
	uint32_t width;
	uint32_t scale;
	unicode_table_t unicode_table;
	bool has_unicode_table;
	uint8_t data[];
} psf_font_t;

static int fread_(void *, void *, unsigned);
typedef int (*fseek_t)(void *, long int, int);
typedef int (*fread_t)(void *, void *, unsigned);
typedef int (*fclose_t)(void *);

static fseek_t _fseek;
static fread_t _fread;
static fclose_t _fclose;

static uint8_t unicode_table_init(unicode_table_t *);
static uint8_t psf1_unicode_table_parse(unicode_table_t *, void *);
static uint8_t psf2_unicode_table_parse(unicode_table_t *, void *);
static uint16_t unicode_table_get(unicode_table_t *, uint32_t);
static void unicode_table_exit(unicode_table_t *);

static int kmscon_font_psf_init(struct kmscon_font *out, const char *name,
				unsigned int query_height)
{
	unsigned char magic[4];
	psf_font_t *font = NULL;
	uint32_t flags, glyphs, step, height, width;
	bool has_unicode_table;

	_fseek = (fseek_t)fseek;
	_fread = (fread_t)fread_;
	_fclose = (fclose_t)fclose;

	void *font_file = fopen(name, "rb");
	if (!font_file) {
		log_error("failed open psf font: %s", name);
		return 1;
	}

	FREAD(font_file, &magic, 4, "failed read magic");

	if (IS_GZ_MAGIC(magic)) {
		_fseek(font_file, 0, SEEK_SET);
		font_file = gzdopen(fileno(font_file), "rb");
		if (!font_file) {
			log_error("failed open font as gz: %s", name);
			return 1;
		}
		_fseek = (fseek_t)gzseek;
		_fread = (fread_t)gzread;
		_fclose = (fclose_t)gzclose;
		FREAD(font_file, &magic, 4, "failed read magic");
	}

	if (IS_PSF1_MAGIC(magic)) {
		flags = magic[2];
		glyphs = (flags & PSF1_FLAG_512) ? 512 : 256;
		has_unicode_table = flags & (PSF1_FLAG_TAB | PSF1_FLAG_SEQ);

		width = 8;
		height = magic[3];
		step = height;
	} else if (IS_PSF2_MAGIC(magic)) {
		_fseek(font_file, 12, SEEK_SET);
		FREAD(font_file, &flags, 4, "failed read flags");
		FREAD(font_file, &glyphs, 4, "failed read glyphs");
		FREAD(font_file, &step, 4, "failed read step");
		FREAD(font_file, &height, 4, "failed read height");
		FREAD(font_file, &width, 4, "failed read width");

		has_unicode_table = flags & PSF2_FLAG_TAB;
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

	if (has_unicode_table) {
		if (unicode_table_init(&font->unicode_table)) {
			log_error("failed init unicode table");
			goto err_file;
		}

		if ((IS_PSF2_MAGIC(magic) ? psf2_unicode_table_parse : psf1_unicode_table_parse)(
			    &font->unicode_table, font_file)) {
			log_error("failed parse unicode table");
			goto err_file;
		}
	}
	_fclose(font_file);

	font->scale = (query_height + (height / 2)) / height;
	if (!font->scale)
		font->scale = 1;
	out->data = font;

	font->width = width * font->scale;
	font->height = height * font->scale;
	out->increase_step = height;

	log_notice("using font: %s %dx%d, scale %d, glyphs %d", name, width, height, font->scale,
		   font->glyphs);

	return 0;

err_file:
	_fclose(font_file);
	return 1;
}

static void kmscon_font_psf_destroy(struct kmscon_font *kfont)
{
	psf_font_t *font = kfont->data;
	log_debug("unloading psf font");
	unicode_table_exit(&font->unicode_table);
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

static struct kmscon_glyph *new_glyph(const struct kmscon_font *kfont,
				      struct kmscon_font_attr *attr, uint32_t ch)
{
	struct kmscon_glyph *glyph;
	unsigned int w = kfont->width;
	unsigned int h = kfont->height;
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
			       attr, k == (font->height - 1));

		for (j = 0; j < w; j++) {
			l = j / font->scale;
			glyph->buf.data[i * glyph->buf.width + j] =
				unfold(c & (1 << (font->width - 1 - l)));
		}
	}
	return glyph;
}

static bool kmscon_font_psf_has_glyph(struct kmscon_font *kfont, struct kmscon_font_attr *attr,
				      uint32_t ch)
{
	psf_font_t *font = kfont->data;

	if (font->has_unicode_table)
		ch = unicode_table_get(&font->unicode_table, ch);

	return (ch == FONT_FULL_BLOCK || ch == FONT_VBAR || ch < font->glyphs);
}

static struct kmscon_glyph *kmscon_font_psf_render(struct kmscon_font *kfont,
						   struct kmscon_font_attr *attr, uint32_t ch)
{
	psf_font_t *font = kfont->data;

	if (ch == FONT_FULL_BLOCK)
		ch = 219;
	else if (ch == FONT_VBAR)
		ch = 179;

	if (font->has_unicode_table)
		ch = unicode_table_get(&font->unicode_table, ch);

	if (ch >= font->glyphs)
		return new_glyph(kfont, attr, '?');

	return new_glyph(kfont, attr, ch);
}

uint8_t unicode_table_init(unicode_table_t *self)
{
	self->cap = 512;
	self->size = 0;
	self->data = malloc(sizeof(unicode_entry_t) * self->cap);

	if (!self->data)
		return 1;

	return 0;
}

static uint8_t unicode_table_add(unicode_table_t *self, uint32_t code, uint16_t ch)
{
	self->data[self->size] = (unicode_entry_t){.glyph = ch, .code = code};

	if ((++self->size) >= self->cap)
		self->data = realloc(self->data, sizeof(unicode_entry_t) * (self->cap *= 2));

	if (!self->data) {
		log_error("failed realloc unicode table");
		return 1;
	}

	return 0;
}

static int cmp_unicode_entry(const void *__1, const void *__2)
{
	const unicode_entry_t *_1 = __1;
	const unicode_entry_t *_2 = __2;

	if (_1->code < _2->code)
		return -1;
	if (_1->code > _2->code)
		return 1;
	return 0;
}

#define UCODE_SIZE 1024
uint8_t psf1_unicode_table_parse(unicode_table_t *self, void *file)
{
	uint8_t ucode[UCODE_SIZE];
	uint32_t ucode_idx = 0;
	uint32_t ucode_size;
	uint16_t ch = 0;

	union {
		uint16_t val;
		uint8_t map[2];
	} code;

	while ((ucode_size = _fread(file, &ucode, UCODE_SIZE))) {
		while (ucode_idx < ucode_size) {
			code.map[0] = ucode[ucode_idx++];
			code.map[1] = ucode[ucode_idx++];

			if (code.map[0] == 0xff && (code.map[1] == 0xfe || code.map[1] == 0xff)) {
				ch++;
				continue;
			}

			if (unicode_table_add(self, code.val, ch))
				return 1;
		}
		ucode_idx = 0;
	}

	qsort(self->data, self->size, sizeof(unicode_entry_t), cmp_unicode_entry);
	return 0;
}

static uint8_t utf8_len(uint8_t c)
{
	return (c & 0x80) == 0 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
}

uint8_t psf2_unicode_table_parse(unicode_table_t *self, void *file)
{
	uint8_t ucode[UCODE_SIZE];
	uint32_t ucode_idx = 0;
	uint32_t ucode_size;
	uint16_t ch = 0;

	union {
		uint32_t val;
		uint8_t map[4];
	} code;

	while ((ucode_size = _fread(file, &ucode, UCODE_SIZE))) {
		while (ucode_idx < ucode_size) {
			code.val = 0;
			switch ((code.map[0] = ucode[ucode_idx++])) {
			case 0xfe:
			case 0xff:
				ch++;
				break;
			default:
				uint8_t i = utf8_len(code.map[0]);
				if (i) {
					code.val = ((uint32_t)(code.map[0] & (0xFF >> (2 + i)))
						    << i * 6);

					for (; i; i--)
						code.val |= ((uint32_t)(ucode[ucode_idx++] & 0x3f)
							     << (i - 1) * 6);
				}

				if (unicode_table_add(self, code.val, ch))
					return 1;
			}
		}
		ucode_idx = 0;
	}

	qsort(self->data, self->size, sizeof(unicode_entry_t), cmp_unicode_entry);
	return 0;
}

static uint16_t unicode_table_get(unicode_table_t *self, uint32_t code)
{
	uint32_t l = 0, r = self->size;
	while (l < r) {
		uint32_t m = (l + r) / 2;

		if (self->data[m].code == code)
			return self->data[m].glyph;
		if (self->data[m].code < code)
			l = m + 1;
		else
			r = m;
	}
	return 0xffff;
}

static void unicode_table_exit(unicode_table_t *self)
{
	free(self->data);
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
