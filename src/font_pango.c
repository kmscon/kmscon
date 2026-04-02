/*
 * kmscon - Pango font backend
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
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
 * SECTION:font_pango.c
 * @short_description: Pango font backend
 * @include: font.h
 *
 * The pango backend uses pango and freetype2 to render glyphs into memory
 * buffers. It uses a hashmap to cache all rendered glyphs of a single
 * font-face. Therefore, rendering should be very fast. Also, when loading a
 * glyph it pre-renders all common (mostly ASCII) characters, so it can measure
 * the font and return a valid font hight/width.
 *
 * This is a _full_ font backend, that is, it provides every feature you expect
 * from a font renderer. It does glyph substitution if a specific font face does
 * not provide a requested glyph, it does correct font loading, it does
 * italic/bold fonts correctly and more.
 * However, this also means it pulls in a lot of dependencies including glib,
 * pango, freetype2 and more.
 */

#include <errno.h>
#include <glib.h>
#include <libtsm.h>
#include <pango/pango.h>
#include <pango/pangoft2.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "shl_dlist.h"
#include "shl_hashtable.h"
#include "shl_log.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "font_pango"

struct face {
	struct kmscon_font_attr attr;
	struct kmscon_font_attr real_attr;
	unsigned int baseline;
	PangoContext *ctx;
	pthread_mutex_t glyph_lock;
	struct shl_hashtable *glyphs;
};

static pthread_mutex_t manager_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long manager__refcnt;
static PangoFontMap *manager__lib;

static void manager_lock()
{
	pthread_mutex_lock(&manager_mutex);
}

static void manager_unlock()
{
	pthread_mutex_unlock(&manager_mutex);
}

static int manager__ref()
{
	if (!manager__refcnt++) {
		manager__lib = pango_ft2_font_map_new();
		if (!manager__lib) {
			log_warn("cannot create font map");
			--manager__refcnt;
			return -EFAULT;
		}
	}

	return 0;
}

static void manager__unref()
{
	if (!--manager__refcnt) {
		g_object_unref(manager__lib);
		manager__lib = NULL;
	}
}

static int get_glyph(struct face *face, struct kmscon_glyph **out, uint64_t id, const uint32_t *ch,
		     size_t len, const struct kmscon_font_attr *attr)
{
	struct kmscon_glyph *glyph;
	PangoLayout *layout;
	PangoAttrList *attrlist;
	PangoRectangle rec, logical_rec;
	PangoLayoutLine *line;
	FT_Bitmap bitmap;
	unsigned int cwidth;
	size_t ulen, cnt;
	char *val;
	bool res;
	int ret;

	if (!len)
		return -ERANGE;
	cwidth = tsm_ucs4_get_width(*ch);
	if (!cwidth)
		return -ERANGE;

	pthread_mutex_lock(&face->glyph_lock);
	res = shl_hashtable_find(face->glyphs, (void **)&glyph, id);
	pthread_mutex_unlock(&face->glyph_lock);
	if (res) {
		*out = glyph;
		return 0;
	}

	manager_lock();

	layout = pango_layout_new(face->ctx);
	attrlist = pango_layout_get_attributes(layout);
	if (attrlist == NULL) {
		attrlist = pango_attr_list_new();
		pango_layout_set_attributes(layout, attrlist);
		pango_attr_list_unref(attrlist);
	}

	/* render one line only */
	pango_layout_set_height(layout, 0);

	/* no line spacing */
	pango_layout_set_spacing(layout, 0);

	/* underline if requested */
	if (attr->underline)
		pango_attr_list_change(attrlist, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
	else
		pango_attr_list_change(attrlist, pango_attr_underline_new(PANGO_UNDERLINE_NONE));

	/* italic if requested */
	if (attr->italic)
		pango_attr_list_change(attrlist, pango_attr_style_new(PANGO_STYLE_ITALIC));
	else
		pango_attr_list_change(attrlist, pango_attr_style_new(PANGO_STYLE_NORMAL));

	/* bold if requested */
	if (attr->bold)
		pango_attr_list_change(attrlist, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
	else
		pango_attr_list_change(attrlist, pango_attr_weight_new(PANGO_WEIGHT_NORMAL));

	val = tsm_ucs4_to_utf8_alloc(ch, len, &ulen);
	if (!val) {
		ret = -ERANGE;
		goto out_layout;
	}
	pango_layout_set_text(layout, val, ulen);
	free(val);

	cnt = pango_layout_get_line_count(layout);
	if (cnt == 0) {
		ret = -ERANGE;
		goto out_layout;
	}

	line = pango_layout_get_line_readonly(layout, 0);

	pango_layout_line_get_extents(line, &logical_rec, &rec);
	pango_extents_to_pixels(&rec, &logical_rec);

	if (logical_rec.x + logical_rec.width > rec.x + face->real_attr.width)
		cwidth = 2;

	glyph = malloc(sizeof(*glyph) + cwidth * face->real_attr.width * face->real_attr.height);
	if (!glyph) {
		log_error("cannot allocate memory for new glyph");
		ret = -ENOMEM;
		goto out_unlock;
	}
	memset(glyph, 0, sizeof(*glyph) + cwidth * face->real_attr.width * face->real_attr.height);

	glyph->width = cwidth;
	glyph->buf.width = face->real_attr.width * glyph->width;
	glyph->buf.height = face->real_attr.height;
	glyph->buf.stride = glyph->buf.width;

	bitmap.rows = glyph->buf.height;
	bitmap.width = glyph->buf.width;
	bitmap.pitch = glyph->buf.stride;
	bitmap.num_grays = 256;
	bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
	bitmap.buffer = glyph->buf.data;

	pango_ft2_render_layout_line(&bitmap, line, -rec.x, face->baseline);

	pthread_mutex_lock(&face->glyph_lock);
	ret = shl_hashtable_insert(face->glyphs, id, glyph);
	pthread_mutex_unlock(&face->glyph_lock);
	if (ret) {
		log_error("cannot add glyph to hashtable");
		goto out_glyph;
	}

	*out = glyph;
	goto out_layout;

out_glyph:
	free(glyph);
out_layout:
	g_object_unref(layout);
out_unlock:
	manager_unlock();
	return ret;
}

static void free_glyph(void *data)
{
	struct kmscon_glyph *glyph = data;

	free(glyph);
}

/*
 * Print the font that is selected by Pango. You need to take the first glyph
 * of the first line, to have the font that is really used.
 */
static void print_font(PangoLayout *layout)
{
	PangoLayoutLine *lines;
	PangoGlyphItem *pgi;
	PangoFontDescription *desc;
	char *font_name;

	lines = pango_layout_get_line_readonly(layout, 0);
	if (!lines || !lines->runs)
		return;

	pgi = lines->runs->data;
	if (!pgi || !pgi->item || !pgi->item->analysis.font)
		return;

	desc = pango_font_describe(pgi->item->analysis.font);
	if (!desc)
		return;
	font_name = pango_font_description_to_string(desc);
	if (font_name) {
		log_notice("Using font %s\n", font_name);
		free(font_name);
	}
	pango_font_description_free(desc);
}

/*
 * This appends ",monospace," to the font provided in kmscon.conf, so that if the
 * specified font is not found, pango will still look for a monospace font.
 */
static PangoFontDescription *new_pango_description(const char *name)
{
	char *font_name;
	PangoFontDescription *desc;
	int ret;

	ret = asprintf(&font_name, "%s%s", name, ",monospace,");
	if (ret < 0)
		return NULL;

	desc = pango_font_description_from_string(font_name);
	free(font_name);
	return desc;
}

static int manager_get_face(struct face **out, struct kmscon_font_attr *attr)
{
	struct face *face;
	PangoFontDescription *desc;
	PangoLayout *layout;
	PangoRectangle rec;
	int ret, num;
	const char *str;

	manager_lock();

	ret = manager__ref();
	if (ret)
		goto out_unlock;

	face = malloc(sizeof(*face));
	if (!face) {
		log_error("cannot allocate memory for new face");
		ret = -ENOMEM;
		goto err_manager;
	}
	memset(face, 0, sizeof(*face));
	memcpy(&face->attr, attr, sizeof(*attr));

	ret = pthread_mutex_init(&face->glyph_lock, NULL);
	if (ret) {
		log_error("cannot initialize glyph lock");
		goto err_free;
	}

	ret = shl_hashtable_new(&face->glyphs, shl_direct_hash, shl_direct_equal, free_glyph);
	if (ret) {
		log_error("cannot allocate hashtable");
		goto err_lock;
	}

	face->ctx = pango_font_map_create_context(manager__lib);
	pango_context_set_base_dir(face->ctx, PANGO_DIRECTION_LTR);
	pango_context_set_language(face->ctx, pango_language_get_default());

	desc = new_pango_description(attr->name);

	pango_font_description_set_absolute_size(desc, PANGO_SCALE * face->attr.height);
	pango_font_description_set_weight(desc,
					  attr->bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
	pango_font_description_set_style(desc,
					 attr->italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
	pango_font_description_set_variant(desc, PANGO_VARIANT_NORMAL);
	pango_font_description_set_stretch(desc, PANGO_STRETCH_NORMAL);
	pango_font_description_set_gravity(desc, PANGO_GRAVITY_SOUTH);
	pango_context_set_font_description(face->ctx, desc);
	pango_font_description_free(desc);

	/* measure font */
	layout = pango_layout_new(face->ctx);
	pango_layout_set_height(layout, 0);
	pango_layout_set_spacing(layout, 0);
	str = "abcdefghijklmnopqrstuvwxyz"
	      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	      "@!\"$%&/()=?\\}][{°^~+*#'<>|-_.:,;`´";
	num = strlen(str);
	pango_layout_set_text(layout, str, num);
	print_font(layout);
	pango_layout_get_pixel_extents(layout, NULL, &rec);

	memcpy(&face->real_attr, &face->attr, sizeof(face->attr));
	face->real_attr.height = rec.height;
	face->real_attr.width = rec.width / num + 1;
	face->baseline = PANGO_PIXELS_CEIL(pango_layout_get_baseline(layout));
	g_object_unref(layout);

	kmscon_font_attr_normalize(&face->real_attr);
	if (!face->real_attr.height || !face->real_attr.width) {
		log_warning("invalid scaled font sizes");
		ret = -EFAULT;
		goto err_face;
	}
	*out = face;
	ret = 0;
	goto out_unlock;

err_face:
	g_object_unref(face->ctx);
	shl_hashtable_free(face->glyphs);
err_lock:
	pthread_mutex_destroy(&face->glyph_lock);
err_free:
	free(face);
err_manager:
	manager__unref();
out_unlock:
	manager_unlock();
	return ret;
}

static void block_sigchild(void)
{
	// glib creates a thread but we haven't blocked SIGCHLD yet
	// (by signal_new) so it might go to glib instead of our signalfd.
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);
}

static int kmscon_font_pango_init(struct kmscon_font *out, const struct kmscon_font_attr *attr)
{
	struct face *face = NULL;
	int ret;

	memcpy(&out->attr, attr, sizeof(*attr));
	kmscon_font_attr_normalize(&out->attr);

	log_debug("loading pango font %s", out->attr.name);

	block_sigchild();

	ret = manager_get_face(&face, &out->attr);
	if (ret)
		return ret;
	memcpy(&out->attr, &face->real_attr, sizeof(out->attr));

	out->data = face;
	out->increase_step = 1;
	return 0;
}

static void kmscon_font_pango_destroy(struct kmscon_font *font)
{
	struct face *face;

	log_debug("unloading pango font");
	face = font->data;

	manager_lock();

	shl_hashtable_free(face->glyphs);
	pthread_mutex_destroy(&face->glyph_lock);
	g_object_unref(face->ctx);
	free(face);
	manager__unref();

	manager_unlock();
}

static int kmscon_font_pango_render(struct kmscon_font *font, uint64_t id, const uint32_t *ch,
				    size_t len, const struct kmscon_glyph **out)
{
	struct kmscon_glyph *glyph;
	int ret;

	ret = get_glyph(font->data, &glyph, id, ch, len, &font->attr);
	if (ret)
		return ret;

	*out = glyph;
	return 0;
}

static int kmscon_font_pango_render_empty(struct kmscon_font *font, const struct kmscon_glyph **out)
{
	static const uint32_t empty_char = ' ';
	return kmscon_font_pango_render(font, empty_char, &empty_char, 1, out);
}

static int kmscon_font_pango_render_inval(struct kmscon_font *font, const struct kmscon_glyph **out)
{
	static const uint32_t question_mark = '?';
	return kmscon_font_pango_render(font, question_mark, &question_mark, 1, out);
}

struct kmscon_font_ops kmscon_font_pango_ops = {
	.name = "pango",
	.owner = NULL,
	.init = kmscon_font_pango_init,
	.destroy = kmscon_font_pango_destroy,
	.render = kmscon_font_pango_render,
	.render_empty = kmscon_font_pango_render_empty,
	.render_inval = kmscon_font_pango_render_inval,
};
