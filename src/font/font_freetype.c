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
#include "shl/log.h"
#include "shl/misc.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "font_freetype"

struct ft_font {
	FT_Face face;
	char *name;
	/* FontSet and Pattern are used for fallback glyphs */
	FcFontSet *fc;
	FcFontSet *fc_unfiltered;
	FcPattern *pattern;
	FT_Face fallback;
	FcFontSet *fallback_src;
	int fallback_index;
};

struct ft_data {
	FT_Library ft;
	struct ft_font regular;
	struct ft_font bold;
	FcConfig *mono;
	FcConfig *fallback;
};

static void free_ft_font(struct ft_font *ftfont)
{
	if (ftfont->face)
		FT_Done_Face(ftfont->face);
	ftfont->face = NULL;
	free(ftfont->name);
	ftfont->name = NULL;
	if (ftfont->fc)
		FcFontSetDestroy(ftfont->fc);
	ftfont->fc = NULL;
	if (ftfont->fc_unfiltered)
		FcFontSetDestroy(ftfont->fc_unfiltered);
	ftfont->fc_unfiltered = NULL;
	if (ftfont->pattern)
		FcPatternDestroy(ftfont->pattern);
	ftfont->pattern = NULL;
	if (ftfont->fallback)
		FT_Done_Face(ftfont->fallback);
	ftfont->fallback = NULL;
	ftfont->fallback_src = NULL;
	ftfont->fallback_index = -1;
}

static int prepare_face(FT_Library ft, struct ft_font *ftfont)
{
	FcChar8 *path;
	FT_Error err;
	int index = 0;
	FcPattern *pattern;
	int ret = -EINVAL;
	FcChar8 *full_name = NULL;

	pattern = FcFontRenderPrepare(NULL, ftfont->pattern, ftfont->fc->fonts[0]);
	if (!pattern)
		return -EINVAL;

	if (FcPatternGetString(pattern, FC_FULLNAME, 0, &full_name) == FcResultMatch)
		ftfont->name = strdup((char *)full_name);
	else
		ftfont->name = strdup("Unknown");

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

static int load_and_render(FT_Face face, FT_UInt index)
{
	if (FT_Load_Glyph(face, index, FT_LOAD_TARGET_LIGHT)) {
		log_err("Failed to load glyph index %d", index);
		return -EINVAL;
	}
	if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
		log_err("Failed to render glyph index %d", index);
		return -EINVAL;
	}
	return 0;
}

static unsigned int font_get_width(FT_Face face)
{
	FT_UInt index = FT_Get_Char_Index(face, 'W');

	if (load_and_render(face, index))
		return 0;

	return max(face->glyph->advance.x >> 6, face->glyph->bitmap.width);
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

static FT_Error request_size(FT_Face face, unsigned int height)
{
	// Request a size of half a pixel less, as freetype draws some characters with 1px more.
	FT_Size_RequestRec request = {FT_SIZE_REQUEST_TYPE_CELL, 0, (height << 6) - 32, 0, 0};
	FT_Error ret;

	ret = FT_Request_Size(face, &request);
	if (ret)
		log_warn("Freetype failed to set size to %d, error %d", height, ret);
	return ret;
}

static void compute_font_size(struct ft_font *ftfont, unsigned int query_height,
			      unsigned int *width, unsigned int *height)
{
	FT_Face face = ftfont->face;

	/* Special case for bitmap fonts, which can't be scaled */
	if (face->num_fixed_sizes) {
		int bitmap_index = bitmap_font_select_size(face, query_height);

		FT_Select_Size(face, bitmap_index);
		*width = font_get_width(face);
		*height = face->available_sizes[bitmap_index].height;
		log_debug("bitmap font size %dx%d", *width, *height);
	} else {
		if (request_size(face, query_height))
			return;
		*width = font_get_width(face);
		*height = (face->size->metrics.height >> 6);
	}
}

static int prepare_font(struct ft_data *ftdata, struct ft_font *ftfont, const char *name,
			unsigned int height, bool bold, struct kmscon_font *font)
{
	FcResult result;
	bool is_default = strcmp(name, KMSCON_FONT_DEFAULT_NAME) == 0;
	FcConfig *cfg;

	ftfont->pattern = FcNameParse((const FcChar8 *)name);
	if (!ftfont->pattern)
		return -EINVAL;

	FcPatternAddInteger(ftfont->pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_NORMAL);
	FcPatternAddDouble(ftfont->pattern, FC_PIXEL_SIZE, (double)height);

	FcPatternAddInteger(ftfont->pattern, FC_SPACING, FC_CHARCELL);
	FcPatternAddInteger(ftfont->pattern, FC_SPACING, FC_MONO);
	FcPatternAddInteger(ftfont->pattern, FC_SPACING, FC_DUAL);

	/*
	 * For the default "monospace" request, select from the monospace-filtered
	 * config so proportional fonts can't break column alignment. When the user
	 * explicitly configured a font, select from the unfiltered config instead:
	 * some usable monospace-ish fonts (e.g. Noto Sans Mono CJK) are not tagged
	 * with FC_SPACING and would otherwise be filtered out.
	 */
	cfg = is_default ? ftdata->mono : ftdata->fallback;

	if (!FcConfigSubstitute(cfg, ftfont->pattern, FcMatchPattern)) {
		log_err("%s: failed to do config substitution", name);
		goto err;
	}

	FcDefaultSubstitute(ftfont->pattern);

	ftfont->fc = FcFontSort(cfg, ftfont->pattern, FcTrue, NULL, &result);
	if (result != FcResultMatch) {
		log_err("%s: failed to match font", name);
		goto err;
	}

	/*
	 * Keep an unfiltered set around so glyphs that no monospace font covers
	 * (e.g. CJK) can still be rendered on the default config, instead of tofu.
	 * Only needed when the primary set is the filtered one; an explicitly
	 * configured font already matched against the unfiltered config.
	 */
	if (is_default)
		ftfont->fc_unfiltered =
			FcFontSort(ftdata->fallback, ftfont->pattern, FcTrue, NULL, &result);

	if (prepare_face(ftdata->ft, ftfont))
		goto err;

	ftfont->fallback_index = -1;
	ftfont->fallback = NULL;
	ftfont->fallback_src = NULL;
	return 0;
err:
	free_ft_font(ftfont);
	return -EINVAL;
}

static FcBool _fc_monospace_font_filter_func(const FcPattern *pat, void *user_data)
{
	int spacing;

	if (FcPatternGetInteger(pat, FC_SPACING, 0, &spacing) == FcResultMatch) {
		if (spacing == FC_CHARCELL || spacing == FC_MONO || spacing == FC_DUAL)
			return FcTrue;
	}
	/* non-monospace font doesn't have FC_SPACING property */
	return FcFalse;
}

static int kmscon_font_freetype_init(struct kmscon_font *out, const char *name, unsigned int height)
{
	struct ft_data *ftf;
	FT_Error err;
	unsigned int bold_width, bold_height;

	ftf = calloc(1, sizeof(*ftf));
	if (!ftf)
		return -ENOMEM;

	err = FT_Init_FreeType(&ftf->ft);
	if (err != 0) {
		log_err("Failed to initialize FreeType\n");
		goto err_free;
	}
	FcConfigSetFontSetFilter(NULL, _fc_monospace_font_filter_func, NULL, NULL);
	ftf->mono = FcConfigReference(FcConfigGetCurrent());
	ftf->fallback = FcInitLoadConfigAndFonts();
	if (!ftf->mono || !ftf->fallback) {
		log_err("Failed to load fontconfig configuration\n");
		goto err_done;
	}
	if (prepare_font(ftf, &ftf->regular, name, height, false, out))
		goto err_done;

	compute_font_size(&ftf->regular, height, &out->width, &out->height);
	if (out->width == 0 || out->height == 0)
		goto err_free_reg;

	if (prepare_font(ftf, &ftf->bold, name, height, true, out))
		goto err_free_reg;

	compute_font_size(&ftf->bold, height, &bold_width, &bold_height);

	if (out->width != bold_width || out->height != bold_height)
		log_warn("Bold and regular font don't have the same dimension");

	out->width = max(out->width, bold_width);
	out->height = max(out->height, bold_height);

	out->increase_step = 1;
	out->data = ftf;

	log_notice("Using [%s] / [%s] size %d -> cell size %dx%d\n", ftf->regular.name,
		   ftf->bold.name, height, out->width, out->height);
	return 0;

err_free_reg:
	free_ft_font(&ftf->regular);
err_done:
	if (ftf->mono)
		FcConfigDestroy(ftf->mono);
	if (ftf->fallback)
		FcConfigDestroy(ftf->fallback);
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
	if (ftf->mono)
		FcConfigDestroy(ftf->mono);
	if (ftf->fallback)
		FcConfigDestroy(ftf->fallback);
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

static void copy_mono(struct video_buffer *buf, FT_GlyphSlot glyph, unsigned int ascender,
		      bool underline)
{
	FT_Bitmap *map = &glyph->bitmap;
	int top = ascender - glyph->bitmap_top;
	int left = glyph->bitmap_left;
	uint8_t *src = map->buffer;
	uint8_t *dst = buf->data;
	int i, j, w, h;

	if (top + map->rows > buf->height)
		top = buf->height - map->rows;
	if (top < 0)
		top = 0;
	if (left < 0)
		left = 0;

	w = min(buf->width - left, map->width);
	h = min(buf->height - top, map->rows);

	for (i = 0; i < h; i++) {
		for (j = 0; j < w; j++)
			dst[j + top * buf->width + left] =
				!!(src[j / 8] & (1 << (7 - (j % 8)))) * 0xff;

		dst += buf->width;
		src += map->pitch;
	}
	if (underline)
		for (j = 0; j < buf->width; j++)
			buf->data[(buf->height - 1) * buf->width + j] = 0xff;
}

static void draw_underline(struct video_buffer *buf, FT_Face face)
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
			buf->data[i * buf->width + j] = 0xff;
}

static void copy_glyph(struct video_buffer *buf, FT_Face face, FT_Bitmap *map, bool underline)
{
	int top = (face->size->metrics.ascender >> 6) - face->glyph->bitmap_top;
	int left = face->glyph->bitmap_left;
	int width, height;
	int left_src = 0;
	int top_src = 0;
	int i;
	uint8_t *dst;

	if (!map->width || !map->rows)
		goto underline;

	if (top < 0) {
		top_src = -top;
		top = 0;
	}
	height = min((int)(buf->height - top), (int)(map->rows - top_src));

	if (left + map->width > buf->width)
		left -= ((left + map->width) - buf->width) / 2;

	if (left < 0) {
		left_src = -left;
		left = 0;
	}
	width = min((int)(buf->width - left), (int)(map->width - left_src));

	if (width <= 0 || height <= 0)
		goto underline;

	dst = buf->data + left + top * buf->width;
	for (i = 0; i < height; i++) {
		memcpy(dst, &map->buffer[(i + top_src) * map->pitch + left_src], width);
		dst += buf->width;
	}

underline:
	if (underline)
		draw_underline(buf, face);
}

static struct kmscon_glyph *render_glyph(struct kmscon_font *font, FT_Face face, FT_UInt index,
					 uint32_t ch, const struct kmscon_font_attr *attr)
{
	unsigned int cwidth;
	struct kmscon_glyph *glyph;

	cwidth = tsm_ucs4_get_width(ch);
	if (!cwidth)
		return NULL;

	if (load_and_render(face, index))
		return NULL;

	cwidth = glyph_is_wide(face->glyph, font->width) ? 2 : cwidth;
	glyph = calloc(1, sizeof(*glyph) + cwidth * font->width * font->height);
	if (!glyph) {
		log_error("cannot allocate memory for new glyph");
		return NULL;
	}

	glyph->double_width = cwidth == 2;
	glyph->buf.width = font->width * cwidth;
	glyph->buf.height = font->height;

	if (face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
		copy_mono(&glyph->buf, face->glyph, face->size->metrics.ascender >> 6,
			  attr->underline);
	else
		copy_glyph(&glyph->buf, face, &face->glyph->bitmap, attr->underline);

	return glyph;
}

static void select_font_size(struct kmscon_font *font, FT_Face face)
{
	int height = font->height;

	/* Special case for bitmap fonts, which can't be scaled */
	if (face->num_fixed_sizes) {
		int bitmap_index = bitmap_font_select_size(face, height);

		FT_Select_Size(face, bitmap_index);
		return;
	}
	/* scale down to a size that fits the glyph height */
	do {
		if (request_size(face, height)) {
			log_warn("failed to set size to %d", height);
			return;
		}
		height--;
	} while (face->size->metrics.height >> 6 > font->height && height > 10);
}

static FT_Face prepare_tmp_face(FT_Library ft, FcPattern *base, FcFontSet *fc, int fallback)
{
	FcChar8 *path;
	FT_Error err;
	int index = 0;
	FcPattern *pattern;
	FT_Face face = NULL;
	FcChar8 *full_name = NULL;

	pattern = FcFontRenderPrepare(NULL, base, fc->fonts[fallback]);
	if (!pattern)
		return NULL;

	FcPatternGetString(pattern, FC_FULLNAME, 0, &full_name);

	if (FcPatternGetString(pattern, FC_FILE, 0, &path) != FcResultMatch)
		goto err_pattern;

	if (FcPatternGetInteger(pattern, FC_INDEX, 0, &index) != FcResultMatch)
		log_warn("%s: failed to get face index", path);

	log_debug("Loading fallback font %s %s", (char *)full_name, (char *)path);

	err = FT_New_Face(ft, (char *)path, index, &face);
	if (err)
		face = NULL;

err_pattern:
	FcPatternDestroy(pattern);
	return face;
}

static int get_fallback(uint32_t ch, FcFontSet *fc)
{
	int i;

	for (i = 0; fc && i < fc->nfont; i++) {
		FcCharSet *cs;
		if (FcPatternGetCharSet(fc->fonts[i], FC_CHARSET, 0, &cs) == FcResultMatch)
			if (FcCharSetHasChar(cs, ch))
				return i;
	}
	return -ENOENT;
}

/*
 * Find a font covering @ch, following fontconfig's own ranking so the
 * locale-preferred font wins (e.g. Noto Sans Mono CJK for CJK locales).
 *
 * For the default request that means the unfiltered set (fc_unfiltered): the
 * monospace filter drops usable fonts that fontconfig doesn't tag as monospace
 * (Noto Sans Mono CJK), and searching the filtered set first would instead pick
 * whatever monospace-tagged font merely happens to sit higher in it. The soft
 * FC_SPACING preference on the pattern still ranks real monospace fonts first,
 * so a proportional font is only ever reached when nothing else covers @ch.
 * When the user configured a font explicitly, fc is already the unfiltered sort.
 */
static int find_fallback(struct ft_font *ftf, uint32_t ch, FcFontSet **fc_out)
{
	FcFontSet *set = ftf->fc_unfiltered ? ftf->fc_unfiltered : ftf->fc;
	int index = get_fallback(ch, set);

	if (index >= 0) {
		*fc_out = set;
		return index;
	}

	return -ENOENT;
}

static bool kmscon_font_freetype_has_glyph(struct kmscon_font *font, struct kmscon_font_attr *attr,
					   uint32_t ch)
{
	struct ft_data *ftd = font->data;
	struct ft_font *ftfont = attr->bold ? &ftd->bold : &ftd->regular;
	FT_UInt glyph_index = FT_Get_Char_Index(ftfont->face, ch);
	FcFontSet *set;

	if (glyph_index)
		return true;

	return find_fallback(ftfont, ch, &set) >= 0;
}

static struct kmscon_glyph *kmscon_font_freetype_render(struct kmscon_font *font,
							struct kmscon_font_attr *attr, uint32_t ch)
{
	struct ft_data *ftd = font->data;
	struct ft_font *ftfont = attr->bold ? &ftd->bold : &ftd->regular;
	FT_UInt glyph_index = FT_Get_Char_Index(ftfont->face, ch);
	FcFontSet *set;
	int fallback_index;

	if (glyph_index)
		return render_glyph(font, ftfont->face, glyph_index, ch, attr);

	/* Fallback, if the glyph is not found in the regular font */
	fallback_index = find_fallback(ftfont, ch, &set);
	if (fallback_index < 0)
		return NULL;

	if (ftfont->fallback_index != fallback_index || ftfont->fallback_src != set) {
		if (ftfont->fallback)
			FT_Done_Face(ftfont->fallback);
		ftfont->fallback = NULL;
		ftfont->fallback_index = fallback_index;
		ftfont->fallback_src = set;
	}

	if (!ftfont->fallback) {
		ftfont->fallback = prepare_tmp_face(ftd->ft, ftfont->pattern, set, fallback_index);
		if (!ftfont->fallback)
			return NULL;
		select_font_size(font, ftfont->fallback);
	}

	glyph_index = FT_Get_Char_Index(ftfont->fallback, ch);
	if (!glyph_index)
		return NULL;
	return render_glyph(font, ftfont->fallback, glyph_index, ch, attr);
}

struct kmscon_font_ops kmscon_font_freetype_ops = {
	.name = "freetype",
	.owner = NULL,
	.init = kmscon_font_freetype_init,
	.destroy = kmscon_font_freetype_destroy,
	.has_glyph = kmscon_font_freetype_has_glyph,
	.render = kmscon_font_freetype_render,
};
