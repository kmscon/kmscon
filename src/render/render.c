/*
 * kmscon - Render
 *
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * SECTION:render
 * @short_description: Render
 * @include: render_internal.h
 *
 * TODO
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include "font/font.h"
#include "render_internal.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "shl/register.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "render"

static struct shl_register render_reg = SHL_REGISTER_INIT(render_reg);

static inline void render_destroy(void *data)
{
	const struct render_ops *ops = data;

	shl_module_unref(ops->owner);
}

/**
 * render_register:
 * @ops: Render operations and name for new backend
 *
 * This register a new render backend with operations set to @ops. The name
 * @ops->name must be valid.
 *
 * The first font that is registered automatically becomes the default and
 * fallback. So make sure you register a safe fallback as first backend.
 * If this is unregistered, the next in the list becomes the default
 * and fallback.
 *
 * Returns: 0 on success, negative error code on failure
 */
SHL_EXPORT
int render_register(const struct render_ops *ops)
{
	int ret;

	if (!ops)
		return -EINVAL;

	log_debug("register render backend %s", ops->name);

	ret = shl_register_add_cb(&render_reg, ops->name, (void *)ops, render_destroy);
	if (ret) {
		log_error("cannot register render backend %s: %d", ops->name, ret);
		return ret;
	}

	shl_module_ref(ops->owner);
	return 0;
}

/**
 * render_unregister:
 * @ops: Operations of the backend
 *
 * This unregisters the render-backend registered with @ops. If it is not
 * registered, nothing is done.
 */
SHL_EXPORT
void render_unregister(const struct render_ops *ops)
{
	if (!ops)
		return;

	log_debug("unregister backend %s", ops->name);
	shl_register_remove(&render_reg, ops->name);
}

static int new_render(struct render *render, const char *backend)
{
	struct shl_register_record *record;
	const char *name = backend ? backend : "<default>";

	render->ref = 1;

	if (backend)
		record = shl_register_find(&render_reg, backend);
	else
		record = shl_register_first(&render_reg);

	if (!record) {
		log_error("requested backend '%s' not found", name);
		return -ENOENT;
	}

	render->record = record;
	render->ops = record->data;

	if (render->ops->init) {
		int ret;

		ret = render->ops->init(render);
		if (ret) {
			log_warning("backend %s cannot create renderer %d", name, ret);
			shl_register_record_unref(record);
			return ret;
		}
	}
	return 0;
}

/**
 * render_new:
 * @out: A pointer to the new render is stored here
 * @backend: Backend to use or NULL for default backend
 * @orientation: Orientation to use for output
 *
 * Returns: 0 on success, error code on failure
 */
int render_new(struct render **out, const char *backend, enum Orientation orientation,
	       struct display *disp)
{
	struct render *text;
	int ret;

	if (!out || !disp)
		return -EINVAL;

	text = calloc(1, sizeof(*text));
	if (!text) {
		log_error("cannot allocate memory for new text-renderer");
		return -ENOMEM;
	}

	text->orientation = orientation;

	ret = new_render(text, backend);
	if (ret) {
		if (backend)
			ret = new_render(text, NULL);
		if (ret)
			goto err_free;
	}
	text->disp = disp;
	display_ref(disp);

	log_debug("using: be: %s", text->ops->name);
	*out = text;
	return 0;

err_free:
	free(text);
	return ret;
}

/**
 * render_ref:
 * @text: Valid render object
 *
 * This increases the reference count of @text by one.
 */
void render_ref(struct render *text)
{
	if (!text || !text->ref)
		return;

	++text->ref;
}

/**
 * render_unref:
 * @text: Valid render object
 *
 * This decreases the reference count of @text by one. If it drops to zero, the
 * object is freed.
 */
void render_unref(struct render *text)
{
	if (!text || !text->ref || --text->ref)
		return;

	log_debug("freeing render");
	render_unset(text);
	display_unref(text->disp);

	if (text->ops->destroy)
		text->ops->destroy(text);
	shl_register_record_unref(text->record);
	free(text);
}

/**
 * render_set:
 * @rdr: Valid render object
 * @font: font object
 *
 * This makes the render @rdr use the font @font. You
 * can drop your reference to the font after calling this.
 * This calls render_unset() first to remove all previous associations.
 * None of the arguments can be NULL!
 * If this function fails then you must assume that no font will be set
 * and the object is invalid.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int render_set(struct render *rdr, struct kmscon_font *font)
{
	int ret;

	if (!rdr || !font)
		return -EINVAL;

	if (rdr->font == font)
		return 0;

	render_unset(rdr);
	rdr->font = font;

	if (rdr->ops->set) {
		ret = rdr->ops->set(rdr);
		if (ret) {
			rdr->font = NULL;
			return ret;
		}
	}
	kmscon_font_ref(rdr->font);
	return 0;
}

/**
 * render_unset():
 * @rdr: render object
 *
 * This redos render_set() by dropping the internal references to the font
 * and invalidating the object. You need to call render_set()
 * again to make use of this render.
 * This is automatically called when the render is destroyed.
 */
void render_unset(struct render *rdr)
{
	if (!rdr || !rdr->font)
		return;

	if (rdr->ops->unset)
		rdr->ops->unset(rdr);

	kmscon_font_unref(rdr->font);
	rdr->font = NULL;
	rdr->cols = 0;
	rdr->rows = 0;
	rdr->rendering = false;
}

/**
 * render_resize:
 * @rdr: valid render object
 * @cols: number of columns
 * @rows: number of rows
 *
 * After setting the arguments with render_set(), the renderer will compute
 * the number of columns/rows of the console that it can display on the screen.
 * But in case of multi-screen setup the actual size of the terminal might be
 * smaller, so call this function to set the current terminal size.
 */
void render_resize(struct render *rdr, unsigned int cols, unsigned int rows)
{
	if (!rdr || !cols || cols > rdr->max_cols || !rows || rows > rdr->max_rows)
		return;
	if (rdr->ops->resize)
		rdr->ops->resize(rdr, cols, rows);
}

unsigned int render_get_width(struct render *rdr)
{
	if (!rdr)
		return 0;
	if (rdr->orientation == OR_NORMAL || rdr->orientation == OR_UPSIDE_DOWN)
		return display_get_width(rdr->disp);
	else
		return display_get_height(rdr->disp);
}

unsigned int render_get_height(struct render *rdr)
{
	if (!rdr)
		return 0;
	if (rdr->orientation == OR_NORMAL || rdr->orientation == OR_UPSIDE_DOWN)
		return display_get_height(rdr->disp);
	else
		return display_get_width(rdr->disp);
}

unsigned int render_get_cols(struct render *rdr, unsigned int font_width)
{
	if (!rdr || !font_width)
		return 0;

	return render_get_width(rdr) / font_width;
}

unsigned int render_get_rows(struct render *rdr, unsigned int font_height)
{
	if (!rdr || !font_height)
		return 0;

	return render_get_height(rdr) / font_height;
}

/**
 * render_get_orientation:
 * @rdr: valid render object
 *
 * With a valid @rdr passed it, this returns the currently active orientation
 * of the output/screen. Possible values are:
 *
 *   - OR_NORMAL
 *   - OR_RIGHT
 *   - OR_UPSIDE_DOWN
 *   - OR_LEFT
 *
 * Returns: Current orientation enum or OR_NORMAL if @rdr is invalid
 */
enum Orientation render_get_orientation(struct render *rdr)
{
	if (!rdr)
		return OR_NORMAL;

	return rdr->orientation;
}

/* The name of the backend */
SHL_EXPORT
const char *render_get_name(struct render *rdr)
{
	if (!rdr)
		return NULL;

	return rdr->ops->name;
}

/* The current font width */
SHL_EXPORT
unsigned int render_get_font_width(struct render *rdr)
{
	if (!rdr || !rdr->font)
		return 0;

	return rdr->font->width;
}

/* The current font height */
SHL_EXPORT
unsigned int render_get_font_height(struct render *rdr)
{
	if (!rdr || !rdr->font)
		return 0;

	return rdr->font->height;
}

/**
 * render_rotate:
 * @rdr: valid render object
 * @orientation: enum value representing the desired output-orientation
 *
 * Update the rotation/orientation of the render. It can be one of:
 *
 *   - OR_NORMAL
 *   - OR_RIGHT
 *   - OR_UPSIDE_DOWN
 *   - OR_LEFT
 *
 * Returns: 0 on success, negative error code on failure.
 */
int render_rotate(struct render *rdr, enum Orientation orientation)
{
	if (rdr->ops->rotate)
		return rdr->ops->rotate(rdr, orientation);
	return 0;
}

/**
 * render_prepare:
 * @rdr: valid render object
 * @attr: glyph attributes
 *
 * This starts a rendering-round. When rendering a console via a render,
 * you have to call this first, then render all your glyphs via
 * render_draw() and finally use render_render(). If you modify this
 * render during rendering or if you activate different OpenGL contexts in
 * between, you need to restart rendering by calling render_prepare() again
 * and redoing everything from the beginning.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int render_prepare(struct render *rdr, struct tsm_attr *attr, bool blinking)
{
	int ret = 0;

	if (!rdr || !rdr->font || !rdr->disp)
		return -EINVAL;

	rdr->rendering = true;
	rdr->blinking = blinking;
	if (rdr->ops->prepare)
		ret = rdr->ops->prepare(rdr, attr);
	if (ret)
		rdr->rendering = false;

	return ret;
}

static bool is_cursor_blinking(enum tsm_cursor_style style)
{
	return (!style || style & 1);
}

static bool is_underline(enum tsm_cursor_style style)
{
	return (style == TSM_CURSOR_UNDERLINE_BLINK || style == TSM_CURSOR_UNDERLINE_STEADY);
}

static bool is_block(enum tsm_cursor_style style)
{
	return (style == TSM_CURSOR_DEFAULT || style == TSM_CURSOR_BLOCK_BLINK ||
		style == TSM_CURSOR_BLOCK_STEADY);
}

static bool is_vbar(enum tsm_cursor_style style)
{
	return (style == TSM_CURSOR_VBAR_BLINK || style == TSM_CURSOR_VBAR_STEADY);
}

/**
 * render_draw:
 * @rdr: valid render object
 * @vte: valid tsm vte
 *
 * This draw all cells in the screen.
 *
 * Returns: 0 on success or negative error code if this glyph couldn't be drawn.
 */
int render_draw(struct render *rdr, struct tsm_vte *vte, bool cursor_blink)
{
	const struct tsm_cell *cells;
	struct render_cursor cursor = {0};
	enum tsm_cursor_style style;
	bool visible;

	if (!rdr || !vte)
		return -EINVAL;

	cells = tsm_vte_render(vte);
	if (!cells)
		return -ENOMEM;
	visible = tsm_vte_get_cursor(vte, &cursor.x, &cursor.y);
	style = tsm_vte_get_cursor_style(vte);

	if (cursor.x < rdr->cols && cursor.y < rdr->rows) {
		unsigned offset = cursor.x + cursor.y * rdr->cols;

		cursor.visible = visible;
		cursor.cell.fg = cells[offset].fg;
		cursor.cell.bg = cells[offset].bg;
		if (is_cursor_blinking(style))
			cursor.visible = cursor.visible && !cursor_blink;
		if (is_underline(style)) {
			cursor.cell.attr =
				(cells[offset].attr & TSM_ATTR_UNDERLINE) ^ TSM_ATTR_UNDERLINE;
			cursor.cell.ch = cells[offset].ch;
		} else if (is_block(style)) {
			cursor.cell.fg = cells[offset].bg;
			cursor.cell.bg = cells[offset].fg;
			cursor.cell.ch = cells[offset].ch;
		} else if (is_vbar(style))
			cursor.cell.ch = FONT_VBAR;
	}
	return rdr->ops->draw(rdr, cells, &cursor);
}

/**
 * render_draw_pointer:
 * @rdr: valid render object
 * @x: X-position of the center of the pointer in pixel
 * @y: Y-position of the center of the pointer in pixel
 *
 * This draws a single I glyph at the requested position. The position is a
 * a pixel position! You must precede this call with render_prepare().
 * Use this function to feed the mouse pointer into the rendering pipeline
 * and finally call render_render().
 *
 * Returns: 0 on success or negative error code if it couldn't be drawn.
 */
int render_draw_pointer(struct render *rdr, unsigned int x, unsigned int y)
{
	if (!rdr || !rdr->rendering || !rdr->ops->draw_pointer)
		return -EINVAL;

	return rdr->ops->draw_pointer(rdr, x, y);
}

/**
 * render_render:
 * @rdr: valid render object
 *
 * This does the final rendering round after render_prepare() has been
 * called and all glyphs were sent to the renderer via render_draw().
 *
 * Returns: 0 on success, negative error on failure.
 */
int render_render(struct render *rdr)
{
	int ret = 0;

	if (!rdr || !rdr->rendering)
		return -EINVAL;

	if (rdr->ops->render)
		ret = rdr->ops->render(rdr);
	rdr->rendering = false;

	return ret;
}

/**
 * render_show_cursor:
 * @rdr: valid render object
 * @x: X position in pixels, in the text area
 * @y: Y position in pixels, in the text area
 *
 * Move the HW cursor of the display to this position, which is clamped to the
 * text area, and rotated like the render.
 */
SHL_EXPORT
void render_show_cursor(struct render *rdr, int32_t x, int32_t y)
{
	int32_t sw, sh;

	if (!rdr || !rdr->font)
		return;

	sw = rdr->cols * FONT_WIDTH(rdr);
	sh = rdr->rows * FONT_HEIGHT(rdr);
	if (sw > 0 && x >= sw)
		x = sw - 1;
	if (sh > 0 && y >= sh)
		y = sh - 1;

	switch (rdr->orientation) {
	default:
	case OR_NORMAL:
		display_show_cursor(rdr->disp, x, y);
		break;
	case OR_UPSIDE_DOWN:
		display_show_cursor(rdr->disp, sw - x, sh - y);
		break;
	case OR_RIGHT:
		display_show_cursor(rdr->disp, sh - y, x);
		break;
	case OR_LEFT:
		display_show_cursor(rdr->disp, y, sw - x);
		break;
	}
}
