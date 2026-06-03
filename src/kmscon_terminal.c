/*
 * kmscon - Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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

/*
 * Terminal
 * A terminal gets assigned an input stream and several output objects and then
 * runs a fully functional terminal emulation on it.
 */

#include <errno.h>
#include <inttypes.h>
#include <libtsm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "conf.h"
#include "font/font.h"
#include "input/input.h"
#include "kmscon_conf.h"
#include "kmscon_issue.h"
#include "kmscon_seat.h"
#include "kmscon_terminal.h"
#include "pty.h"
#include "render/text.h"
#include "shl/dlist.h"
#include "shl/eloop.h"
#include "shl/log.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "terminal"

struct screen {
	struct shl_dlist list;
	struct kmscon_terminal *term;
	struct display *disp;
	struct kmscon_text *txt;

	bool swapping;
	bool pending;
	bool hw_cursor;
	bool enabled;
};

struct kmscon_pointer {
	bool visible;
	bool select;
	int32_t x;
	int32_t y;
	unsigned int posx;
	unsigned int posy;
	char *copy;
	int copy_len;
};

struct kmscon_terminal {
	unsigned long ref;
	struct ev_eloop *eloop;
	struct input *input;
	bool opened;
	bool awake;

	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;
	struct kmscon_session *session;

	struct shl_dlist screens;
	unsigned int min_cols;
	unsigned int min_rows;

	struct tsm_screen *console;
	struct tsm_vte *vte;
	struct kmscon_pty *pty;
	struct ev_fd *ptyfd;

	struct kmscon_font_attr font_attr;
	struct kmscon_font *font;

	struct kmscon_pointer pointer;
};

static int font_set(struct kmscon_terminal *term);

static void coord_to_cell(struct kmscon_terminal *term, int32_t x, int32_t y, unsigned int *posx,
			  unsigned int *posy)
{
	int fw = term->font->attr.width;
	int fh = term->font->attr.height;
	int w = tsm_screen_get_width(term->console);
	int h = tsm_screen_get_height(term->console);

	*posx = x / fw;
	*posy = y / fh;

	if (*posx >= w)
		*posx = w - 1;

	if (*posy >= h)
		*posy = h - 1;
}

static void draw_pointer(struct screen *scr)
{
	if (!scr->term->pointer.visible || scr->hw_cursor)
		return;

	kmscon_text_draw_pointer(scr->txt, scr->term->pointer.x, scr->term->pointer.y);
}

static inline uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
	return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/*
 * Thin I-beam: 1px stem, proportional top/bottom serifs, 1px outline halo.
 * The shape is built into a boolean mask, then rendered in a single pass
 * that fills white on the shape and a dark outline on any pixel adjacent
 * to it (8-connected).
 */
static uint32_t *generate_ibeam_cursor(unsigned int font_height, unsigned int *width,
				       unsigned int *height, bool rotate)
{
	unsigned int h, w;
	int i, x, y, ny, nx, thk;
	bool *shape, near;
	uint32_t white, outline;
	uint32_t *pixels;

	h = font_height > 8 ? font_height : 8;
	h = min(h, VIDEO_CURSOR_MAX_SIZE);
	thk = 1 + (h / 16);
	w = 2 * (h / 6) + 3 * thk;

	white = argb(255, 255, 255, 255);
	outline = argb(220, 0, 0, 0);

	shape = calloc(w, h * sizeof(*shape));
	if (!shape)
		return NULL;

	pixels = calloc(w, h * sizeof(*pixels));
	if (!pixels) {
		free(shape);
		return NULL;
	}

	if (rotate) {
		unsigned tmp = w;
		w = h;
		h = tmp;
		/* vertical stem */
		for (x = thk; x < w - thk; x++)
			for (i = 0; i < thk; i++)
				shape[x + ((h - thk) / 2) * w + i * w] = true;

		/* Top and bottom serifs */
		for (y = thk; y < h - thk; y++) {
			for (i = 0; i < thk; i++) {
				shape[i + thk + y * w] = true;
				shape[w - i - 1 - thk + y * w] = true;
			}
		}
	} else {
		/* vertical stem */
		for (y = thk; y < h - thk; y++)
			for (i = 0; i < thk; i++)
				shape[(w - thk) / 2 + y * w + i] = true;

		/* Top and bottom serifs */
		for (x = thk; x < w - thk; x++) {
			for (i = 0; i < thk; i++) {
				shape[w * (i + thk) + x] = true;
				shape[(h - i - 1 - thk) * w + x] = true;
			}
		}
	}

	/* White fill on the shape, dark halo on 8-connected neighbors */
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			if (shape[y * w + x]) {
				pixels[y * w + x] = white;
				continue;
			}
			near = false;
			for (ny = y - thk; ny <= y + thk && !near; ny++) {
				for (nx = x - thk; nx <= x + thk && !near; nx++) {
					if (ny >= 0 && ny < (int)h && nx >= 0 && nx < (int)w &&
					    shape[ny * w + nx])
						near = true;
				}
			}
			if (near)
				pixels[y * w + x] = outline;
		}
	}
	free(shape);
	*width = w;
	*height = h;
	return pixels;
}

static void setup_hw_cursor(struct screen *scr)
{
	struct kmscon_terminal *term = scr->term;
	bool rotate = scr->txt->orientation == OR_LEFT || scr->txt->orientation == OR_RIGHT;
	unsigned int beam_h;
	unsigned int beam_w;
	uint32_t *pixels;
	int ret;

	pixels = generate_ibeam_cursor(term->font->attr.height, &beam_w, &beam_h, rotate);
	if (!pixels)
		return;

	ret = display_setup_cursor(scr->disp, pixels, beam_w, beam_h, beam_w / 2, beam_h / 2);
	free(pixels);

	if (ret) {
		log_debug("HW cursor not available for display %s, using software",
			  display_name(scr->disp));
		scr->hw_cursor = false;
	} else {
		log_debug("HW cursor enabled for display %s", display_name(scr->disp));
		scr->hw_cursor = true;
	}
}

static void refresh_hw_cursor(struct screen *scr)
{
	if (!scr->hw_cursor)
		return;

	display_destroy_cursor(scr->disp);
	setup_hw_cursor(scr);
}

static void disable_screen(struct screen *scr)
{
	int ret;

	log_debug("Disabling screen %s", display_name(scr->disp));
	if (scr->swapping)
		scr->pending = true;
	else {
		log_info("Disabling screen %s", display_name(scr->disp));
		scr->pending = false;
		display_clear(scr->disp, 0, 0, 0);
		ret = display_swap(scr->disp);
		if (ret) {
			if (ret != -EBUSY)
				log_warning("cannot swap display [%s] %d", display_name(scr->disp),
					    ret);
			else {
				log_debug("display [%s] is swapping", display_name(scr->disp));
				scr->pending = true;
			}
		}
		scr->swapping = true;
	}
	scr->enabled = false;
}

static void do_redraw_screen(struct screen *scr)
{
	struct tsm_screen_attr attr;
	int ret;

	if (!scr->term->awake || !kmscon_session_get_foreground(scr->term->session))
		return;

	if (!scr->enabled) {
		/* make sure to clear unused screen */
		if (scr->pending)
			disable_screen(scr);
		return;
	}

	scr->pending = false;

	tsm_vte_get_def_attr(scr->term->vte, &attr);
	kmscon_text_prepare(scr->txt, &attr);
	tsm_screen_draw(scr->term->console, kmscon_text_draw_cb, scr->txt);
	draw_pointer(scr);
	kmscon_text_render(scr->txt);

	ret = display_swap(scr->disp);
	if (ret) {
		if (ret != -EBUSY)
			log_warning("cannot swap display [%s] %d", display_name(scr->disp), ret);
		return;
	}

	scr->swapping = true;
}

static void redraw_screen(struct screen *scr)
{
	if (!scr->term->awake || !scr->enabled)
		return;

	if (scr->swapping)
		scr->pending = true;
	else
		do_redraw_screen(scr);
}

static void redraw_all(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	if (!term->awake)
		return;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		redraw_screen(scr);
	}
}

static bool has_kms_display(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		if (display_is_drm(scr->disp))
			return true;
	}
	return false;
}

/*
 * Align the pointer maximum to the minimum width and height of all screens
 * according to their orientation, as kmscon only support mirroring, and one
 * terminal size for all screens.
 */
static void update_pointer_max_all(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;
	unsigned int max_x = INT_MAX;
	unsigned int max_y = INT_MAX;
	unsigned int sw, sh;

	if (!term->awake)
		return;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		if (!scr->enabled)
			continue;

		if (scr->txt->orientation == OR_NORMAL || scr->txt->orientation == OR_UPSIDE_DOWN) {
			sw = display_get_width(scr->disp);
			sh = display_get_height(scr->disp);
		} else {
			sw = display_get_height(scr->disp);
			sh = display_get_width(scr->disp);
		}
		if (!sw || !sh)
			continue;

		if (sw < max_x)
			max_x = sw;
		if (sh < max_y)
			max_y = sh;
	}
	if (max_x < INT_MAX && max_y < INT_MAX)
		input_set_pointer_max(term->input, max_x, max_y);
}

static void redraw_all_text(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	if (!term->awake)
		return;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		if (display_is_swapping(scr->disp))
			scr->swapping = true;
		redraw_screen(scr);
	}
}

static void display_pageflip(void *unused, void *unused2, void *data)
{
	struct screen *scr = data;

	scr->swapping = false;
	if (scr->pending)
		do_redraw_screen(scr);
}

static void osc_event(struct tsm_vte *vte, const char *osc_string, size_t osc_len, void *data)
{
	struct kmscon_terminal *term = data;

	if (strcmp(osc_string, "setBackground") == 0) {
		log_info("Got OSC setBackground");
		kmscon_session_set_background(term->session);
	} else if (strcmp(osc_string, "setForeground") == 0) {
		log_info("Got OSC setForeground");
		kmscon_session_set_foreground(term->session);
	}
}

static void bell_event(struct tsm_vte *vte, void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->conf->bell)
		return;

	kmscon_session_bell(term->session);
}

static void led_event(struct tsm_vte *vte, unsigned int leds, void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_session_set_leds(term->session, leds & TSM_VTE_LED_SCROLL_LOCK,
				leds & TSM_VTE_LED_NUM_LOCK, leds & TSM_VTE_LED_CAPS_LOCK);
}

static void mouse_event(struct tsm_vte *vte, enum tsm_mouse_track_mode track_mode,
			bool track_pixels, void *data)
{
	struct kmscon_terminal *term = data;

	term->pointer.select = false;
	tsm_screen_selection_reset(term->console);
}

/*
 * We support multiple monitors per terminal. In clone mode, we use the smallest cols/rows that are
 * provided so wider monitors will have black margins.
 */
static bool terminal_update_size_clone(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;
	unsigned int min_cols = UINT_MAX;
	unsigned int min_rows = UINT_MAX;

	shl_dlist_for_each(iter, &term->screens)
	{
		unsigned int cols, rows;
		scr = shl_dlist_entry(iter, struct screen, list);
		cols = kmscon_text_get_cols(scr->txt);
		if (cols && cols < min_cols)
			min_cols = cols;

		rows = kmscon_text_get_rows(scr->txt);
		if (rows && rows < min_rows)
			min_rows = rows;
	}
	if (min_cols == UINT_MAX || min_rows == UINT_MAX)
		return false;

	if (min_cols == term->min_cols && min_rows == term->min_rows)
		return false;

	term->min_cols = min_cols;
	term->min_rows = min_rows;

	return true;
}

/*
 * In largest mode, we use the largest cols/rows that are
 * provided so smaller monitors will be disabled.
 */
static bool terminal_update_size_largest(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;
	unsigned int rows, cols, cells;
	unsigned int max_cells = 0;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		rows = kmscon_text_get_rows(scr->txt);
		cols = kmscon_text_get_cols(scr->txt);
		cells = rows * cols;
		if (cells > max_cells) {
			max_cells = cells;
			term->min_cols = cols;
			term->min_rows = rows;
		}
	}
	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		rows = kmscon_text_get_rows(scr->txt);
		cols = kmscon_text_get_cols(scr->txt);
		if (rows != term->min_rows || cols != term->min_cols)
			disable_screen(scr);
		else if (!scr->enabled) {
			log_info("Enabling screen %s", display_name(scr->disp));
			scr->enabled = true;
		}
	}
	return max_cells > 0;
}

static bool terminal_update_size(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;
	bool ret;

	if (term->conf->multi_monitor && !strcmp(term->conf->multi_monitor, "largest")) {
		ret = terminal_update_size_largest(term);
	} else {
		ret = terminal_update_size_clone(term);
	}
	if (!ret)
		return false;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		if (scr->enabled)
			kmscon_text_resize(scr->txt, term->min_cols, term->min_rows);
	}
	return true;
}

static void terminal_update_size_notify(struct kmscon_terminal *term)
{
	if (terminal_update_size(term)) {
		tsm_screen_resize(term->console, term->min_cols, term->min_rows);
		kmscon_pty_resize(term->pty, term->min_cols, term->min_rows);
		redraw_all(term);
	}
}

static int font_set(struct kmscon_terminal *term)
{
	int ret;
	struct kmscon_font *font;
	struct shl_dlist *iter;
	struct screen *scr;

	ret = kmscon_font_find(&font, &term->font_attr, term->conf->font_engine);
	if (ret)
		return ret;

	kmscon_font_unref(term->font);
	term->font = font;

	term->min_cols = 0;
	term->min_rows = 0;
	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);

		ret = kmscon_text_set(scr->txt, font, scr->disp);
		if (ret)
			log_warning("cannot change text-renderer font: %d", ret);
		refresh_hw_cursor(scr);
	}
	terminal_update_size_notify(term);
	return 0;
}

static void rotate_cw_screen(struct screen *scr)
{
	unsigned int orientation = kmscon_text_get_orientation(scr->txt);
	orientation = (orientation + 1) % (OR_LEFT + 1);
	kmscon_text_rotate(scr->txt, orientation);
	refresh_hw_cursor(scr);
}

static void rotate_cw_all(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		rotate_cw_screen(scr);
	}
	terminal_update_size_notify(term);
	update_pointer_max_all(term);
}

static void rotate_ccw_screen(struct screen *scr)
{
	unsigned int orientation = kmscon_text_get_orientation(scr->txt);
	if (orientation == OR_NORMAL)
		orientation = OR_LEFT;
	else
		orientation -= 1;
	kmscon_text_rotate(scr->txt, orientation);
	refresh_hw_cursor(scr);
}

static void rotate_ccw_all(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		rotate_ccw_screen(scr);
	}
	terminal_update_size_notify(term);
	update_pointer_max_all(term);
}

int terminal_add_display(struct kmscon_terminal *term, struct display *disp)
{
	struct shl_dlist *iter;
	struct screen *scr;
	int ret;
	const char *be;
	bool opengl;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		if (scr->disp == disp)
			return 0;
	}

	scr = malloc(sizeof(*scr));
	if (!scr) {
		log_error("cannot allocate memory for display %p", disp);
		return -ENOMEM;
	}
	memset(scr, 0, sizeof(*scr));
	scr->term = term;
	scr->disp = disp;
	scr->enabled = true;

	ret = display_register_pageflip(scr->disp, display_pageflip, scr);
	if (ret) {
		log_error("cannot register display callback: %d", ret);
		goto err_free;
	}

	opengl = display_has_opengl(scr->disp);
	if (opengl)
		be = "gltex";
	else
		be = "bbulk";

	ret = kmscon_text_new(&scr->txt, be, term->conf->rotate);
	if (ret) {
		log_error("cannot create text-renderer");
		goto err_cb;
	}

	ret = kmscon_text_set(scr->txt, term->font, scr->disp);
	if (ret) {
		log_error("cannot set text-renderer parameters");
		goto err_text;
	}

	shl_dlist_link(&term->screens, &scr->list);

	log_notice("Display [%s] with backend [%s] text renderer [%s] font engine [%s]\n",
		   display_name(disp), display_backend_name(disp), scr->txt->ops->name,
		   term->font->ops->name);

	log_debug("added display %p to terminal %p", disp, term);

	if (term->conf->mouse && !term->conf->soft_cursor)
		setup_hw_cursor(scr);

	terminal_update_size_notify(term);
	kmscon_text_resize(scr->txt, term->min_cols, term->min_rows);
	update_pointer_max_all(term);
	display_ref(scr->disp);
	do_redraw_screen(scr);
	return 0;

err_text:
	kmscon_text_unref(scr->txt);
err_cb:
	display_unregister_pageflip(scr->disp, display_pageflip, scr);
err_free:
	free(scr);
	return ret;
}

static void free_screen(struct screen *scr, bool update)
{
	struct kmscon_terminal *term = scr->term;

	log_debug("destroying terminal screen %p", scr);
	if (scr->hw_cursor)
		display_destroy_cursor(scr->disp);
	shl_dlist_unlink(&scr->list);
	kmscon_text_unref(scr->txt);
	display_unregister_pageflip(scr->disp, display_pageflip, scr);
	display_unref(scr->disp);
	free(scr);

	if (!update)
		return;

	update_pointer_max_all(term);
	terminal_update_size_notify(term);
}

void terminal_rm_display(struct kmscon_terminal *term, struct display *disp)
{
	struct shl_dlist *iter;
	struct screen *scr;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		if (scr->disp == disp)
			break;
	}

	if (iter == &term->screens)
		return;

	log_debug("removed display %p from terminal %p", disp, term);
	free_screen(scr, true);
}

static void input_event(struct input *input, struct input_key_event *ev, void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->opened || !term->awake || ev->handled ||
	    !kmscon_session_get_foreground(term->session))
		return;

	// reset mouse selection on keypress
	tsm_screen_selection_reset(term->console);

	if (conf_grab_matches(term->conf->grab_scroll_up, ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_up(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_scroll_down, ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_down(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_page_up, ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_page_up(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_page_down, ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_page_down(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_zoom_in, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (term->font_attr.height + term->font->increase_step < term->font_attr.height)
			return;

		term->font_attr.height += term->font->increase_step;
		if (font_set(term))
			term->font_attr.height -= term->font->increase_step;
		return;
	}
	if (conf_grab_matches(term->conf->grab_zoom_out, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (term->font_attr.height <= term->font->increase_step)
			return;

		term->font_attr.height -= term->font->increase_step;
		if (font_set(term))
			term->font_attr.height += term->font->increase_step;
		return;
	}
	if (conf_grab_matches(term->conf->grab_rotate_cw, ev->mods, ev->num_syms, ev->keysyms)) {
		rotate_cw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_rotate_ccw, ev->mods, ev->num_syms, ev->keysyms)) {
		rotate_ccw_all(term);
		ev->handled = true;
		return;
	}

	/* TODO: xkbcommon supports multiple keysyms, but it is currently
	 * unclear how this feature will be used. There is no keymap, which
	 * uses this, yet. */
	if (ev->num_syms > 1)
		return;

	if (tsm_vte_handle_keyboard(term->vte, ev->keysyms[0], ev->ascii, ev->mods,
				    ev->codepoints[0])) {
		tsm_screen_sb_reset(term->console);
		redraw_all(term);
		ev->handled = true;
	}
}

static void start_selection(struct tsm_screen *console, unsigned int x, unsigned int y)
{
	tsm_screen_selection_reset(console);
	tsm_screen_selection_start(console, x, y);
}

static void update_selection(struct tsm_screen *console, unsigned int x, unsigned int y)
{
	tsm_screen_selection_target(console, x, y);
}

static void free_selection(struct kmscon_terminal *term)
{
	if (!term->pointer.copy)
		return;
	free(term->pointer.copy);
	term->pointer.copy = NULL;
	term->pointer.copy_len = 0;
}

static void copy_selection(struct kmscon_terminal *term)
{
	free_selection(term);
	term->pointer.copy_len = tsm_screen_selection_copy(term->console, &term->pointer.copy);
}

static void forward_pointer_event(struct kmscon_terminal *term, struct input_pointer_event *ev)
{
	unsigned int event;
	unsigned int button;
	int32_t wheel;

	button = ev->button;
	wheel = ev->wheel;
	if (term->conf->natural_scrolling)
		wheel = -wheel;

	switch (ev->event) {
	case POINTER_MOVED:
		event = TSM_MOUSE_EVENT_MOVED;
		/* In mouse tracking protocol, motion with button pressed uses button+32 */
		if (ev->pressed && button <= 2) {
			button += 32;
		}
		break;
	case POINTER_BUTTON:
		if (ev->pressed)
			event = TSM_MOUSE_EVENT_PRESSED;
		else
			event = TSM_MOUSE_EVENT_RELEASED;
		break;
	case POINTER_WHEEL:
		/* Convert wheel events to button 4 (scroll up) or 5 (scroll down) */
		event = TSM_MOUSE_EVENT_PRESSED;
		if (wheel > 0)
			button = 4; /* Scroll up */
		else
			button = 5; /* Scroll down */
		break;
	default:
		return;
	}
	tsm_vte_handle_mouse(term->vte, term->pointer.posx, term->pointer.posy, term->pointer.x,
			     term->pointer.y, button, event, 0);
}

static void handle_pointer_button(struct kmscon_terminal *term, struct input_pointer_event *ev)
{
	switch (ev->button) {
	case 0:
		if (ev->pressed) {
			if (ev->double_click) {
				tsm_screen_selection_word(term->console, term->pointer.posx,
							  term->pointer.posy);
				copy_selection(term);
				term->pointer.select = false;
			} else {
				term->pointer.select = true;
				start_selection(term->console, term->pointer.posx,
						term->pointer.posy);
			}
		} else {
			if (term->pointer.select)
				copy_selection(term);
			term->pointer.select = false;
		}
		break;
	case 1:
		term->pointer.select = false;
		tsm_screen_selection_reset(term->console);
		break;
	case 2:
		if (ev->pressed) {
			if (term->pointer.copy && term->pointer.copy_len)
				tsm_vte_paste(term->vte, term->pointer.copy);
			tsm_screen_selection_reset(term->console);
		}
	}
}

static void text_show_cursor(struct kmscon_text *txt, int32_t x, int32_t y)
{
	unsigned int sw = txt->cols * FONT_WIDTH(txt);
	unsigned int sh = txt->rows * FONT_HEIGHT(txt);

	switch (txt->orientation) {
	default:
	case OR_NORMAL:
		display_show_cursor(txt->disp, x, y);
		break;
	case OR_UPSIDE_DOWN:
		display_show_cursor(txt->disp, sw - x, sh - y);
		break;
	case OR_RIGHT:
		display_show_cursor(txt->disp, sh - y, x);
		break;
	case OR_LEFT:
		display_show_cursor(txt->disp, y, sw - x);
		break;
	}
}

static void hw_cursor_show(struct kmscon_terminal *term, int32_t x, int32_t y)
{
	struct shl_dlist *iter;
	struct screen *scr;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		if (scr->hw_cursor)
			text_show_cursor(scr->txt, x, y);
	}
}

static void hw_cursor_hide(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		if (scr->hw_cursor)
			display_hide_cursor(scr->disp);
	}
}

static void pointer_event(struct input *input, struct input_pointer_event *ev, void *data)
{
	struct kmscon_terminal *term = data;

	if (ev->event == POINTER_MOVED) {
		term->pointer.x = ev->pointer_x;
		term->pointer.y = ev->pointer_y;

		coord_to_cell(term, term->pointer.x, term->pointer.y, &term->pointer.posx,
			      &term->pointer.posy);
		term->pointer.visible = true;
		hw_cursor_show(term, ev->pointer_x, ev->pointer_y);
	}

	if (tsm_vte_get_mouse_mode(term->vte) != TSM_MOUSE_TRACK_DISABLE &&
	    ev->event != POINTER_SYNC) {
		forward_pointer_event(term, ev);
		return;
	}

	switch (ev->event) {
	default:
		break;
	case POINTER_MOVED:
		if (term->pointer.select)
			update_selection(term->console, term->pointer.posx, term->pointer.posy);
		break;
	case POINTER_BUTTON:
		handle_pointer_button(term, ev);
		break;
	case POINTER_WHEEL:
		tsm_screen_selection_reset(term->console);
		if (term->conf->natural_scrolling != (ev->wheel > 0))
			tsm_screen_sb_up(term->console, 3);
		else
			tsm_screen_sb_down(term->console, 3);
		break;
	case POINTER_SYNC:
		redraw_all(term);
		break;
	case POINTER_HIDE_TIMEOUT:
		tsm_screen_selection_reset(term->console);
		term->pointer.visible = false;
		hw_cursor_hide(term);
		break;
	}
}

static void rm_all_screens(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	while ((iter = term->screens.next) != &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		free_screen(scr, false);
	}

	term->min_cols = 0;
	term->min_rows = 0;
}

static int terminal_open(struct kmscon_terminal *term)
{
	int ret;
	unsigned short width, height;

	if (term->opened)
		return -EALREADY;

	tsm_vte_hard_reset(term->vte);
	width = tsm_screen_get_width(term->console);
	height = tsm_screen_get_height(term->console);
	ret = kmscon_pty_open(term->pty, width, height, has_kms_display(term));
	if (ret)
		return ret;

	term->opened = true;
	if (term->conf->issue)
		kmscon_issue_write(term->vte, term->pty, term->conf->issue_path);

	update_pointer_max_all(term);
	redraw_all(term);
	return 0;
}

static void terminal_close(struct kmscon_terminal *term)
{
	kmscon_pty_close(term->pty);
	term->opened = false;
}

void terminal_refresh_displays(struct kmscon_terminal *term)
{
	if (term->pointer.visible)
		hw_cursor_show(term, term->pointer.x, term->pointer.y);
	redraw_all_text(term);
}

void terminal_activate(struct kmscon_terminal *term)
{
	term->awake = true;
	if (!term->opened)
		terminal_open(term);
	if (term->pointer.visible)
		hw_cursor_show(term, term->pointer.x, term->pointer.y);
	redraw_all_text(term);
}

void terminal_deactivate(struct kmscon_terminal *term)
{
	term->awake = false;
	hw_cursor_hide(term);
}

void terminal_destroy(struct kmscon_terminal *term)
{
	log_debug("free terminal object %p", term);

	terminal_close(term);
	rm_all_screens(term);
	input_unregister_pointer_cb(term->input, pointer_event, term);
	input_unregister_key_cb(term->input, input_event, term);
	ev_eloop_rm_fd(term->ptyfd);
	kmscon_pty_unref(term->pty);
	kmscon_font_unref(term->font);
	tsm_vte_unref(term->vte);
	tsm_screen_unref(term->console);
	input_unref(term->input);
	ev_eloop_unref(term->eloop);
	free_selection(term);
	free(term);
}

static void pty_input(struct kmscon_pty *pty, const char *u8, size_t len, void *data)
{
	struct kmscon_terminal *term = data;

	if (!len) {
		terminal_close(term);
		terminal_open(term);
	} else {
		tsm_vte_input(term->vte, u8, len);
		redraw_all(term);
	}
}

static void pty_event(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_dispatch(term->pty);
}

static void write_event(struct tsm_vte *vte, const char *u8, size_t len, void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_write(term->pty, u8, len);
}

struct kmscon_terminal *terminal_new(struct kmscon_session *session, unsigned int vtnr,
				     struct conf_ctx *conf_ctx, struct ev_eloop *eloop,
				     struct input *input, const char *seat_name)
{
	struct kmscon_terminal *term;
	int ret;

	term = malloc(sizeof(*term));
	if (!term)
		return NULL;

	memset(term, 0, sizeof(*term));
	term->ref = 1;
	term->session = session;
	term->eloop = eloop;
	term->input = input;
	shl_dlist_init(&term->screens);

	term->conf_ctx = conf_ctx;
	term->conf = conf_ctx_get_mem(term->conf_ctx);

	strncpy(term->font_attr.name, term->conf->font_name, KMSCON_FONT_MAX_NAME - 1);
	term->font_attr.height = term->conf->font_size;

	ret = tsm_screen_new(&term->console, log_llog, NULL);
	if (ret)
		goto err_free;
	tsm_screen_set_max_sb(term->console, term->conf->sb_size);

	ret = tsm_vte_new(&term->vte, term->console, write_event, term, log_llog, NULL);
	if (ret)
		goto err_con;

	tsm_vte_set_backspace_sends_delete(term->vte, term->conf->backspace_delete);

	tsm_vte_set_osc_cb(term->vte, osc_event, (void *)term);
	tsm_vte_set_mouse_cb(term->vte, mouse_event, (void *)term);
	tsm_vte_set_bell_cb(term->vte, bell_event, (void *)term);
	tsm_vte_set_led_cb(term->vte, led_event, (void *)term);

	ret = tsm_vte_set_palette(term->vte, term->conf->palette);
	if (ret)
		goto err_vte;

	ret = tsm_vte_set_custom_palette(term->vte, term->conf->custom_palette);
	if (ret)
		goto err_vte;

	ret = font_set(term);
	if (ret)
		goto err_vte;

	ret = kmscon_pty_new(&term->pty, pty_input, term);
	if (ret)
		goto err_font;

	ret = kmscon_pty_set_conf(term->pty, term->conf->term, "truecolor", term->conf->argv,
				  seat_name, vtnr, term->conf->reset_env,
				  term->conf->backspace_delete);
	if (ret)
		goto err_pty;

	ret = ev_eloop_new_fd(term->eloop, &term->ptyfd, kmscon_pty_get_fd(term->pty), EV_READABLE,
			      pty_event, term);
	if (ret)
		goto err_pty;

	ret = input_register_key_cb(term->input, input_event, term);
	if (ret)
		goto err_ptyfd;

	if (term->conf->mouse) {
		ret = input_register_pointer_cb(term->input, pointer_event, term);
		if (ret)
			goto err_input;
	}

	ev_eloop_ref(term->eloop);
	input_ref(term->input);
	log_debug("new terminal object %p", term);
	return term;

err_input:
	input_unregister_key_cb(term->input, input_event, term);
err_ptyfd:
	ev_eloop_rm_fd(term->ptyfd);
err_pty:
	kmscon_pty_unref(term->pty);
err_font:
	kmscon_font_unref(term->font);
err_vte:
	tsm_vte_unref(term->vte);
err_con:
	tsm_screen_unref(term->console);
err_free:
	free(term);
	return NULL;
}
