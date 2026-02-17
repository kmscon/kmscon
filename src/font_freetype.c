/*
 * kmscon - Freetype font backend
 *
 * Copyright (c) 2026 Red Hat.
 * Author: Jocelyn Falempe <jfalempe@redhat.com>
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

#include <fontconfig/fontconfig.h>
#include <freetype2/freetype/freetype.h>
#include FT_FREETYPE_H
#include <libtsm.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "font_freetype"

struct ft_font {
	FT_Library ft;
	FT_Face face;
};

static void print_font_name(FcPattern *pattern)
{
	FcChar8 *full_name = NULL;

	if (FcPatternGetString(pattern, FC_FULLNAME, 0, &full_name) != FcResultMatch)
		log_warn("failed to get full font name");
	else
		log_notice("Using font %s\n", full_name);
}

static FcPattern *lookup_font(const FcChar8 *name, bool bold, int size)
{
	FcPattern *pattern = FcNameParse(name);
	FcPattern *ret = NULL;
	FcFontSet *set;
	FcResult result;

	if (!pattern)
		return NULL;

	FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_NORMAL);
	FcPatternAddDouble(pattern, FC_SIZE, (double)size);

	if (!FcConfigSubstitute(NULL, pattern, FcMatchPattern)) {
		log_err("%s: failed to do config substitution", name);
		goto err_pattern;
	}

	FcDefaultSubstitute(pattern);

	set = FcFontSort(NULL, pattern, FcTrue, NULL, &result);
	if (result != FcResultMatch) {
		log_err("%s: failed to match font", name);
		goto err_pattern;
	}
	ret = FcFontRenderPrepare(NULL, pattern, set->fonts[0]);

err_pattern:
	FcPatternDestroy(pattern);
	return ret;
}

static FT_Error setup_font(struct ft_font *ftf, const struct kmscon_font_attr *attr)
{
	FcPattern *pattern = lookup_font((const FcChar8 *)attr->name, attr->bold, attr->height);
	FcChar8 *path;
	FT_Error err;
	int index = 0;

	if (!pattern)
		return -1;

	print_font_name(pattern);

	if (FcPatternGetString(pattern, FC_FILE, 0, &path) != FcResultMatch)
		return -1;

	if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) != FcResultMatch)
		log_warn("%s: failed to get face index", path);

	log_debug("Loading font %s", (char *)path);

	err = FT_New_Face(ftf->ft, (char *)path, index, &ftf->face);

	FcPatternDestroy(pattern);
	return err;
}

static int font_get_width(FT_Face face)
{
	FT_UInt glyph_index = FT_Get_Char_Index(face, 'M');

	if (FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT))
		return -1;

	if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL))
		return -1;

	return face->glyph->advance.x >> 6;
}

static int bitmap_font_select_size(FT_Face face, int height)
{
	int i, diff, best = 0;
	int min = INT_MAX;

	for (i = 0; i < face->num_fixed_sizes; i++) {
		diff = abs(face->available_sizes[i].height - height);
		if (diff < min) {
			min = diff;
			best = i;
		}
	}
	log_debug("Select bitmap, asked height %d found height %d within %d choices\n", height,
		  face->available_sizes[best].height, face->num_fixed_sizes);
	return best;
}

static int kmscon_font_freetype_init(struct kmscon_font *out, const struct kmscon_font_attr *attr)
{
	struct ft_font *ftf;
	FT_Error err;

	ftf = malloc(sizeof(*ftf));
	if (!ftf)
		return -ENOMEM;
	memset(ftf, 0, sizeof(*ftf));
	memcpy(&out->attr, attr, sizeof(*attr));
	kmscon_font_attr_normalize(&out->attr);

	err = FT_Init_FreeType(&ftf->ft);
	if (err != 0) {
		log_err("Failed to initialize FreeType\n");
		goto err_free;
	}

	err = setup_font(ftf, &out->attr);
	if (err != 0) {
		log_err("Failed to find face FreeType\n");
		goto err_done;
	}

	/* Special case for bitmap fonts, which can't be scaled */
	if (ftf->face->num_fixed_sizes) {
		int bitmap_index = bitmap_font_select_size(ftf->face, out->attr.height);

		FT_Select_Size(ftf->face, bitmap_index);
		out->attr.width = ftf->face->available_sizes[bitmap_index].width;
		out->attr.height = ftf->face->available_sizes[bitmap_index].height;
	} else {
		err = FT_Set_Pixel_Sizes(ftf->face, 0, out->attr.height);
		if (err)
			log_warn("Freetype failed to set size to %d", out->attr.height);
		out->attr.width = font_get_width(ftf->face);
		out->attr.height = (ftf->face->size->metrics.height / 64);
	}
	if (!out->attr.width || !out->attr.height) {
		log_err("Invalid font %dx%d", out->attr.width, out->attr.height);
		goto err_face;
	}
	out->increase_step = 1;
	out->data = ftf;

	log_debug("Font attr %dx%d", out->attr.width, out->attr.height);
	return 0;

err_face:
	FT_Done_Face(ftf->face);
err_done:
	FT_Done_FreeType(ftf->ft);
err_free:
	free(ftf);
	return -EINVAL;
}

static void kmscon_font_freetype_destroy(struct kmscon_font *font)
{
	struct ft_font *ftf = font->data;

	log_debug("unloading freetype font");
	FT_Done_Face(ftf->face);
	FT_Done_FreeType(ftf->ft);
	free(ftf);
	font->data = NULL;
}

/*
 * Returns true if a glyph is wide and needs 2 cells.
 * Take a 20% margin, in case the glyph slightly bleeds on the next cell.
 */
static bool glyph_is_wide(FT_GlyphSlot glyph, int width)
{
	int real_width = glyph->bitmap.width + glyph->bitmap_left;
	return real_width > (width * 6) / 5;
}

static void copy_mono(struct uterm_video_buffer *buf, FT_Bitmap *map, bool underline)
{
	uint8_t *src = map->buffer;
	uint8_t *dst = buf->data;
	int i, j;

	for (i = 0; i < buf->height; i++) {
		for (j = 0; j < buf->width; j++)
			dst[j] = !!(src[j / 8] & (1 << (7 - (j % 8)))) * 0xff;

		dst += buf->stride;
		src += map->pitch;
	}
	if (underline)
		for (j = 0; j < buf->width; j++)
			buf->data[(buf->height - 1) * buf->stride + j] = 0xff;
}

static void draw_underline(struct uterm_video_buffer *buf, FT_Face face)
{
	int i, j;
	int thickness = FT_MulFix(face->underline_thickness, face->size->metrics.y_scale);
	int position = FT_MulFix(face->underline_position, face->size->metrics.y_scale);

	thickness = (thickness + (thickness >> 1)) >> 6;
	position = (face->size->metrics.ascender - position) >> 6;

	if (thickness < 1 || thickness > buf->height / 4)
		thickness = 1;

	if (position + thickness > buf->height)
		position = buf->height - thickness;

	for (i = position; i < position + thickness; i++)
		for (j = 0; j < buf->width; j++)
			buf->data[i * buf->stride + j] = 0xff;
}

static void copy_glyph(struct uterm_video_buffer *buf, FT_Face face, FT_Bitmap *map, bool underline)
{
	int top = (face->size->metrics.ascender >> 6) - face->glyph->bitmap_top;
	int left = face->glyph->bitmap_left;
	int width = min(buf->width, map->width);
	int height = min(buf->height, map->rows);
	int left_src = 0;
	int top_src = 0;
	int i;
	uint8_t *dst;

	if (top + height > buf->height)
		height = buf->height - top;
	if (top < 0) {
		top_src = -top;
		height = min(buf->height, map->rows + top);
		top = 0;
	}

	if (left < 0) {
		left_src = -left;
		width = min(buf->width, map->width + left);
		left = 0;
	}
	if (left + width > buf->width)
		width = buf->width - left;

	dst = buf->data + left + top * buf->stride;
	for (i = 0; i < height; i++) {
		memcpy(dst, &map->buffer[(i + top_src) * map->pitch + left_src], width);
		dst += buf->stride;
	}

	if (underline)
		draw_underline(buf, face);
}

static int kmscon_font_freetype_render(struct kmscon_font *font, uint64_t id, const uint32_t *ch,
				       size_t len, const struct kmscon_glyph **out)
{
	struct ft_font *ftf = font->data;
	FT_Face face = ftf->face;
	struct kmscon_glyph *glyph;
	unsigned int cwidth;
	FT_UInt glyph_index = FT_Get_Char_Index(face, *ch);

	if (!len)
		return -ERANGE;

	cwidth = tsm_ucs4_get_width(*ch);
	if (!cwidth)
		return -ERANGE;

	if (FT_Load_Glyph(face, glyph_index, FT_LOAD_NO_HINTING)) {
		log_err("Failed to load glyph\n");
		return -EINVAL;
	}

	if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
		log_err("Failed to render glyph\n");
		return -EINVAL;
	}

	glyph = malloc(sizeof(*glyph));
	if (!glyph) {
		log_error("cannot allocate memory for new glyph");
		return -ENOMEM;
	}
	memset(glyph, 0, sizeof(*glyph));

	glyph->width = glyph_is_wide(face->glyph, font->attr.width) ? 2 : cwidth;
	glyph->buf.width = font->attr.width * glyph->width;
	glyph->buf.height = font->attr.height;
	glyph->buf.stride = glyph->buf.width;
	glyph->buf.format = UTERM_FORMAT_GREY;

	glyph->buf.data = malloc(glyph->buf.height * glyph->buf.stride);
	if (!glyph->buf.data) {
		log_error("cannot allocate bitmap memory");
		return -ENOMEM;
	}
	memset(glyph->buf.data, 0, glyph->buf.height * glyph->buf.stride);

	if (face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
		copy_mono(&glyph->buf, &face->glyph->bitmap, font->attr.underline);
	else
		copy_glyph(&glyph->buf, face, &face->glyph->bitmap, font->attr.underline);

	*out = glyph;
	return 0;
}

static int kmscon_font_freetype_render_empty(struct kmscon_font *font,
					     const struct kmscon_glyph **out)
{
	static const uint32_t empty_char = ' ';
	return kmscon_font_freetype_render(font, empty_char, &empty_char, 1, out);
}

static int kmscon_font_freetype_render_inval(struct kmscon_font *font,
					     const struct kmscon_glyph **out)
{
	static const uint32_t question_mark = '?';
	return kmscon_font_freetype_render(font, question_mark, &question_mark, 1, out);
}

struct kmscon_font_ops kmscon_font_freetype_ops = {
	.name = "freetype",
	.owner = NULL,
	.init = kmscon_font_freetype_init,
	.destroy = kmscon_font_freetype_destroy,
	.render = kmscon_font_freetype_render,
	.render_empty = kmscon_font_freetype_render_empty,
	.render_inval = kmscon_font_freetype_render_inval,
};
