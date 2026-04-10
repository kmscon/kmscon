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
	FT_Face face;
	/* FontSet and Pattern are used for fallback glyphs */
	FcFontSet *fc;
	FcPattern *pattern;
	FT_Face fallback;
	int fallback_index;
};

struct ft_data {
	FT_Library ft;
	struct ft_font regular;
	struct ft_font bold;
};

static void print_font_name(FcPattern *pattern)
{
	FcChar8 *full_name = NULL;

	if (FcPatternGetString(pattern, FC_FULLNAME, 0, &full_name) != FcResultMatch)
		log_warn("failed to get full font name");
	else
		log_notice("Using font %s\n", full_name);
}

static void free_ft_font(struct ft_font *ftfont)
{
	if (ftfont->face)
		FT_Done_Face(ftfont->face);
	ftfont->face = NULL;
	if (ftfont->fc)
		FcFontSetDestroy(ftfont->fc);
	ftfont->fc = NULL;
	if (ftfont->pattern)
		FcPatternDestroy(ftfont->pattern);
	ftfont->pattern = NULL;
	if (ftfont->fallback)
		FT_Done_Face(ftfont->fallback);
	ftfont->fallback = NULL;
	ftfont->fallback_index = -1;
}

static int prepare_face(FT_Library ft, struct ft_font *ftfont)
{
	FcChar8 *path;
	FT_Error err;
	int index = 0;
	FcPattern *pattern;
	int ret = -EINVAL;

	pattern = FcFontRenderPrepare(NULL, ftfont->pattern, ftfont->fc->fonts[0]);
	if (!pattern)
		return -EINVAL;

	print_font_name(pattern);

	if (FcPatternGetString(pattern, FC_FILE, 0, &path) != FcResultMatch)
		goto err_pattern;

	if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) != FcResultMatch)
		log_warn("%s: failed to get face index", path);

	log_debug("Loading font %s", (char *)path);

	err = FT_New_Face(ft, (char *)path, index, &ftfont->face);
	ret = err ? -EINVAL : 0;

err_pattern:
	FcPatternDestroy(pattern);
	return ret;
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

static int compute_font_size(struct ft_font *ftfont, struct kmscon_font_attr *attr)
{
	FT_Face face = ftfont->face;

	/* Special case for bitmap fonts, which can't be scaled */
	if (face->num_fixed_sizes) {
		int bitmap_index = bitmap_font_select_size(face, attr->height);

		FT_Select_Size(face, bitmap_index);
		attr->width = face->available_sizes[bitmap_index].width;
		attr->height = face->available_sizes[bitmap_index].height;
	} else {
		if (FT_Set_Pixel_Sizes(face, 0, attr->height))
			log_warn("Freetype failed to set size to %d", attr->height);
		attr->width = font_get_width(face);
		attr->height = (face->size->metrics.height >> 6);
	}
	if (!attr->width || !attr->height) {
		log_err("Invalid font %dx%d", attr->width, attr->height);
		free_ft_font(ftfont);
		return -EINVAL;
	}
	return 0;
}

static int prepare_font(FT_Library ft, struct ft_font *ftfont, struct kmscon_font_attr *attr)
{
	FcResult result;

	ftfont->pattern = FcNameParse((const FcChar8 *)attr->name);
	if (!ftfont->pattern)
		return -EINVAL;

	FcPatternAddInteger(ftfont->pattern, FC_WEIGHT,
			    attr->bold ? FC_WEIGHT_BOLD : FC_WEIGHT_NORMAL);
	FcPatternAddDouble(ftfont->pattern, FC_SIZE, (double)attr->height);

	FcPatternAddInteger(ftfont->pattern, FC_SPACING, FC_CHARCELL);
	FcPatternAddInteger(ftfont->pattern, FC_SPACING, FC_MONO);
	FcPatternAddInteger(ftfont->pattern, FC_SPACING, FC_DUAL);

	if (!FcConfigSubstitute(NULL, ftfont->pattern, FcMatchPattern)) {
		log_err("%s: failed to do config substitution", attr->name);
		goto err;
	}

	FcDefaultSubstitute(ftfont->pattern);

	ftfont->fc = FcFontSort(NULL, ftfont->pattern, FcTrue, NULL, &result);
	if (result != FcResultMatch) {
		log_err("%s: failed to match font", attr->name);
		goto err;
	}

	if (prepare_face(ft, ftfont))
		goto err;

	if (compute_font_size(ftfont, attr))
		goto err;

	ftfont->fallback_index = -1;
	ftfont->fallback = NULL;
	return 0;
err:
	free_ft_font(ftfont);
	return -EINVAL;
}

static int kmscon_font_freetype_init(struct kmscon_font *out, const struct kmscon_font_attr *attr)
{
	struct ft_data *ftf;
	struct kmscon_font_attr bold_attr;
	FT_Error err;

	ftf = malloc(sizeof(*ftf));
	if (!ftf)
		return -ENOMEM;
	memset(ftf, 0, sizeof(*ftf));
	kmscon_copy_attr(&out->attr, attr);
	kmscon_font_attr_normalize(&out->attr);
	kmscon_copy_attr(&bold_attr, &out->attr);
	bold_attr.bold = true;

	err = FT_Init_FreeType(&ftf->ft);
	if (err != 0) {
		log_err("Failed to initialize FreeType\n");
		goto err_free;
	}
	if (prepare_font(ftf->ft, &ftf->regular, &out->attr))
		goto err_done;

	if (prepare_font(ftf->ft, &ftf->bold, &bold_attr))
		goto err_free_reg;

	if (out->attr.width != bold_attr.width || out->attr.height != bold_attr.height)
		log_warn("Bold and regular font don't have the same dimension");

	out->attr.width = max(out->attr.width, bold_attr.width);
	out->attr.height = max(out->attr.height, bold_attr.height);

	out->increase_step = 1;
	out->data = ftf;

	log_debug("Font attr %dx%d", out->attr.width, out->attr.height);
	return 0;

err_free_reg:
	free_ft_font(&ftf->regular);
err_done:
	FT_Done_FreeType(ftf->ft);
err_free:
	free(ftf);
	return -EINVAL;
}

static void kmscon_font_freetype_destroy(struct kmscon_font *font)
{
	struct ft_data *ftf = font->data;

	log_debug("unloading freetype font");
	free_ft_font(&ftf->regular);
	free_ft_font(&ftf->bold);
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

	// Round thinkness to nearest integer
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
	int width, height;
	int left_src = 0;
	int top_src = 0;
	int i;
	uint8_t *dst;

	if (!map->width || !map->rows)
		return;

	if (top < 0) {
		top_src = -top;
		top = 0;
	}
	height = min((int)(buf->height - top), (int)(map->rows - top_src));

	if (left < 0) {
		left_src = -left;
		left = 0;
	}
	width = min((int)(buf->width - left), (int)(map->width - left_src));

	if (width <= 0 || height <= 0)
		return;

	dst = buf->data + left + top * buf->stride;
	for (i = 0; i < height; i++) {
		memcpy(dst, &map->buffer[(i + top_src) * map->pitch + left_src], width);
		dst += buf->stride;
	}

	if (underline)
		draw_underline(buf, face);
}

static struct kmscon_glyph *render_glyph(FT_Face face, FT_UInt index, const uint32_t *ch,
					 const struct kmscon_font_attr *attr)
{
	unsigned int cwidth;
	struct kmscon_glyph *glyph;

	cwidth = tsm_ucs4_get_width(*ch);
	if (!cwidth)
		return NULL;

	if (FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING)) {
		log_err("Failed to load glyph\n");
		return NULL;
	}

	if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
		log_err("Failed to render glyph\n");
		return NULL;
	}

	cwidth = glyph_is_wide(face->glyph, attr->width) ? 2 : cwidth;
	glyph = malloc(sizeof(*glyph) + cwidth * attr->width * attr->height);
	if (!glyph) {
		log_error("cannot allocate memory for new glyph");
		return NULL;
	}
	memset(glyph, 0, sizeof(*glyph) + cwidth * attr->width * attr->height);

	glyph->double_width = cwidth == 2;
	glyph->buf.width = attr->width * cwidth;
	glyph->buf.height = attr->height;
	glyph->buf.stride = glyph->buf.width;

	if (face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
		copy_mono(&glyph->buf, &face->glyph->bitmap, attr->underline);
	else
		copy_glyph(&glyph->buf, face, &face->glyph->bitmap, attr->underline);

	return glyph;
}

static void select_font_size(FT_Face face, struct kmscon_font_attr *attr)
{
	int height = attr->height;

	/* Special case for bitmap fonts, which can't be scaled */
	if (face->num_fixed_sizes) {
		int bitmap_index = bitmap_font_select_size(face, attr->height);

		FT_Select_Size(face, bitmap_index);
		return;
	}
	if (FT_Set_Pixel_Sizes(face, 0, height))
		log_warn("Freetype failed to set size to %d", height);

	// Make sure the fallback glyph will fit in our glyph size
	while (face->size->metrics.height >> 6 > attr->height && height > 2) {
		height--;
		FT_Set_Pixel_Sizes(face, 0, height);
	}
}

static FT_Face prepare_tmp_face(FT_Library ft, struct ft_font *font, int fallback)
{
	FcChar8 *path;
	FT_Error err;
	int index = 0;
	FcPattern *pattern;
	FT_Face face = NULL;

	pattern = FcFontRenderPrepare(NULL, font->pattern, font->fc->fonts[fallback]);
	if (!pattern)
		return NULL;

	print_font_name(pattern);

	if (FcPatternGetString(pattern, FC_FILE, 0, &path) != FcResultMatch)
		goto err_pattern;

	if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) != FcResultMatch)
		log_warn("%s: failed to get face index", path);

	log_debug("Loading fallback font %s", (char *)path);

	err = FT_New_Face(ft, (char *)path, index, &face);
	if (err)
		face = NULL;

err_pattern:
	FcPatternDestroy(pattern);
	return face;
}

static int get_fallback(uint32_t ch, struct ft_font *ftf)
{
	int i;

	for (i = 0; i < ftf->fc->nfont; i++) {
		FcCharSet *cs;
		if (FcPatternGetCharSet(ftf->fc->fonts[i], FC_CHARSET, 0, &cs) == FcResultMatch)
			if (FcCharSetHasChar(cs, ch))
				return i;
	}
	return -ENOENT;
}

static struct kmscon_glyph *kmscon_font_freetype_render(struct kmscon_font *font, uint64_t id,
							const uint32_t *ch, size_t len)
{
	struct ft_data *ftd = font->data;
	struct ft_font *ftfont = font->attr.bold ? &ftd->bold : &ftd->regular;
	FT_UInt glyph_index = FT_Get_Char_Index(ftfont->face, *ch);
	int fallback_index;

	if (!len)
		return NULL;

	if (glyph_index)
		return render_glyph(ftfont->face, glyph_index, ch, &font->attr);

	/* Fallback, if the glyph is not found in the regular font */
	fallback_index = get_fallback(*ch, ftfont);
	if (fallback_index < 0)
		return NULL;

	if (ftfont->fallback_index != fallback_index) {
		if (ftfont->fallback)
			FT_Done_Face(ftfont->fallback);
		ftfont->fallback = NULL;
		ftfont->fallback_index = fallback_index;
	}

	if (!ftfont->fallback) {
		ftfont->fallback = prepare_tmp_face(ftd->ft, ftfont, fallback_index);
		if (!ftfont->fallback)
			return NULL;
		select_font_size(ftfont->fallback, &font->attr);
	}

	glyph_index = FT_Get_Char_Index(ftfont->fallback, *ch);
	if (!glyph_index)
		return NULL;
	return render_glyph(ftfont->fallback, glyph_index, ch, &font->attr);
}

struct kmscon_font_ops kmscon_font_freetype_ops = {
	.name = "freetype",
	.owner = NULL,
	.init = kmscon_font_freetype_init,
	.destroy = kmscon_font_freetype_destroy,
	.render = kmscon_font_freetype_render,
};
