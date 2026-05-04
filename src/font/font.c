/*
 * kmscon - Font handling
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
 * SECTION:font
 * @short_description: Font handling
 * @include: font.h
 *
 * The text renderer needs a backend that draws glyphs which then can be shown
 * on the screen. This font handling subsystem provides a very simple API to
 * load arbitrary font-renderer backends. That is, you can choose from
 * in-memory bitmap fonts up to full Unicode compatible font libraries like
 * pango during runtime.
 *
 * This system does not provide any renderer by itself. You need to register one
 * of the available font-renderers first which then is used as backend for this
 * system. kmscon_font_register() and kmscon_font_unregister() can be used to
 * register font-renderers manually.
 *
 * @kmscon_font_attr is used to specify font-attributes for the fonts you want.
 * Please see kmscon_font_find() for more information on font-attributes. This
 * function returns a matching font which then can be used for drawing.
 * kmscon_font_ref()/kmscon_font_unref() are used for reference counting.
 * kmscon_font_render() renders a single unicode glyph and returns the glyph
 * buffer. kmscon_font_drop() frees this buffer again. A kmscon_glyph object
 * contains a memory-buffer with the rendered glyph plus some metrics like
 * height/width but also ascent/descent.
 *
 * Font-backends must take into account that this API must be thread-safe as it
 * is shared between different threads to reduce memory-footprint.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "shl_module.h"
#include "shl_register.h"

#define LOG_SUBSYSTEM "font"

static struct shl_register font_reg = SHL_REGISTER_INIT(font_reg);

static inline void kmscon_font_destroy(void *data)
{
	const struct kmscon_font_ops *ops = data;

	shl_module_unref(ops->owner);
}

/**
 * kmscon_font_register:
 * @ops: Font operations and name for new font backend
 *
 * This register a new font backend with operations set to @ops. The name
 * @ops->name must be valid.
 *
 * The first font that is registered automatically becomes the default font and
 * the fallback font. So make sure you register a safe fallback as first font.
 * If this font is unregistered, the next font in the list becomes the default
 * and fallback font.
 *
 * Returns: 0 on success, negative error code on failure
 */
SHL_EXPORT
int kmscon_font_register(const struct kmscon_font_ops *ops)
{
	int ret;

	if (!ops)
		return -EINVAL;

	log_debug("register font backend %s", ops->name);

	ret = shl_register_add_cb(&font_reg, ops->name, (void *)ops, kmscon_font_destroy);
	if (ret) {
		log_error("cannot register font backend %s: %d", ops->name, ret);
		return ret;
	}

	shl_module_ref(ops->owner);
	return 0;
}

/**
 * kmscon_font_unregister:
 * @name: Name of font backend
 *
 * This unregisters the font-backend that is registered with name @name. If
 * @name is not found, a warning is printed but nothing else is done.
 */
SHL_EXPORT
void kmscon_font_unregister(const char *name)
{
	log_debug("unregister font backend %s", name);
	shl_register_remove(&font_reg, name);
}

static const char *default_font[] = {"freetype", "pango", "unifont", "8x16"};

static int init_font(struct kmscon_font *font, struct shl_register_record *record,
		     const struct kmscon_font_attr *attr)
{
	memset(font, 0, sizeof(*font));
	font->ref = 1;
	font->record = record;
	font->ops = record->data;

	if (font->ops->init)
		return font->ops->init(font, attr);
	return 0;
}

static int try_font(struct kmscon_font *font, const struct kmscon_font_attr *attr,
		    const char *backend)
{
	struct shl_register_record *record = shl_register_find(&font_reg, backend);
	if (!record)
		return -ENOENT;
	return init_font(font, record, attr);
}

static int new_font(struct kmscon_font *font, const struct kmscon_font_attr *attr,
		    const char *backend)
{
	int ret = -ENOENT;

	if (backend)
		ret = try_font(font, attr, backend);

	if (ret == 0)
		return 0;

	for (int i = 0; i < sizeof(default_font) / sizeof(default_font[0]); i++) {
		ret = try_font(font, attr, default_font[i]);
		if (ret == 0)
			return 0;
	}
	/* last resort */
	return init_font(font, shl_register_first(&font_reg), attr);
}

/**
 * kmscon_font_find:
 * @out: A pointer to the new font is stored here
 * @attr: Attribute describing the font
 * @backend: Backend to use or NULL for default backend
 *
 * Lookup a font by the given attributes. It uses the font backend @backend. If
 * it is NULL, the default backend is used. If the given backend cannot find
 * a suitable font, the fallback backend is tried. This backend should always
 * find a suitable font.
 *
 * Stores a pointer to the new font in @out and returns 0. Otherwise, @out is
 * not touched and an error is returned.
 *
 * The attributes in @attr are not always matched. There are even font backends
 * which have only one fixed font and always return this one so you cannot rely
 * on this behavior. That is, this function cannot be used to get an exact
 * match, it rather returns the best matching font.
 * There is currently no need to get an exact match so no API is available to
 * get this. Instead, you should always use the best match and the user must be
 * happy. We do print warnings if no close match can be found, though. The user
 * should read them if they want more information what font fallback was used.
 *
 * If this functions fails, you must not assume that there is another font that
 * might work. Moreover, you must not implement a fallback font yourself as this
 * is already implemented inside of this function! This function fails only due
 * to internal errors like failed memory allocations. If it fails, the chances
 * that you can allocate your own fallback font are pretty small so don't do it.
 *
 * About DPI and Point Sizes:
 * Use a fixed DPI of 72, so point size is the same as height in pixels.
 *
 * Returns: 0 on success, error code on failure
 */
int kmscon_font_find(struct kmscon_font **out, const struct kmscon_font_attr *attr,
		     const char *backend)
{
	struct kmscon_font *font;
	int ret;

	if (!out || !attr)
		return -EINVAL;

	log_debug("searching for: be: %s nm: %s b: %d size %ux%u", backend, attr->name, attr->bold,
		  attr->height, attr->width);

	font = malloc(sizeof(*font));
	if (!font) {
		log_error("cannot allocate memory for new font");
		return -ENOMEM;
	}

	ret = new_font(font, attr, backend);
	if (ret)
		goto err_free;

	log_debug("using: be: %s nm: %s b: %d size %ux%u", font->ops->name, font->attr.name,
		  font->attr.bold, font->attr.height, font->attr.width);
	*out = font;
	return 0;

err_free:
	log_error("No font backend available: %d", ret);
	free(font);
	return ret;
}

/**
 * kmscon_font_ref:
 * @font: Valid font object
 *
 * This increases the reference count of @font by one.
 */
void kmscon_font_ref(struct kmscon_font *font)
{
	if (!font || !font->ref)
		return;

	++font->ref;
}

/**
 * kmscon_font_unref:
 * @font: Valid font object
 *
 * This decreases the reference count of @font by one. If it drops to zero, the
 * object is freed.
 */
void kmscon_font_unref(struct kmscon_font *font)
{
	if (!font || !font->ref || --font->ref)
		return;

	log_debug("freeing font");
	if (font->ops->destroy)
		font->ops->destroy(font);
	shl_register_record_unref(font->record);
	free(font);
}

/**
 * kmscon_font_render:
 * @font: Valid font object
 * @id: Unique ID that identifies @ch globally
 * @ch: Symbol to find a glyph for
 * @len: Length of @ch
 *
 * Renders the glyph for symbol @ch and returns a pointer to the glyph.
 * If the glyph cannot be found or is invalid, NULL is returned.
 *
 * Returns: a new allocated glyph object on success, NULL on failure
 */
SHL_EXPORT
struct kmscon_glyph *kmscon_font_render(struct kmscon_font *font, uint64_t id, const uint32_t *ch,
					size_t len)
{
	uint32_t empty_char = ' ';
	uint32_t replacement_char = 0xfffd;
	uint32_t invalid_char = '?';
	struct kmscon_glyph *glyph;

	if (!font)
		return NULL;

	if (!len) {
		return font->ops->render(font, empty_char, &empty_char, 1);
	}

	glyph = font->ops->render(font, id, ch, len);
	if (!glyph)
		glyph = font->ops->render(font, replacement_char, &replacement_char, 1);
	if (!glyph)
		glyph = font->ops->render(font, invalid_char, &invalid_char, 1);
	return glyph;
}

/**
 * kmscon_font_has_glyph:
 * @font: Valid font object
 * @ch: Symbol to find a glyph for
 * @len: Length of @ch
 *
 * Checks if the font has a glyph for the given symbol @ch.
 *
 * Returns: true if the font has a glyph for the given symbol, false otherwise
 */
SHL_EXPORT
bool kmscon_font_has_glyph(struct kmscon_font *font, const uint32_t *ch, size_t len)
{
	if (!font)
		return false;

	return font->ops->has_glyph(font, ch, len);
}