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
#include <stdlib.h>
#include <string.h>
#include "conf.h"
#include "eloop.h"
#include "font.h"
#include "kmscon_conf.h"
#include "kmscon_seat.h"
#include "kmscon_terminal.h"
#include "pty.h"
#include "shl_dlist.h"
#include "shl_log.h"
#include "text.h"
#include "uterm_input.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "terminal"

struct screen {
	struct shl_dlist list;
	struct kmscon_terminal *term;
	struct uterm_display *disp;
	struct kmscon_text *txt;

	bool swapping;
	bool pending;
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
	struct uterm_input *input;
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
	struct kmscon_font *bold_font;

	struct kmscon_pointer pointer;
};

static void do_clear_margins(struct screen *scr)
{
	unsigned int w, h, sw, sh;
	struct tsm_screen_attr attr;
	int dh, dw;

	sw = uterm_display_get_width(scr->disp);
	sh = uterm_display_get_height(scr->disp);

	if (!sw || !sh)
		return;

	tsm_vte_get_def_attr(scr->term->vte, &attr);

	if (scr->txt->orientation == OR_NORMAL || scr->txt->orientation == OR_UPSIDE_DOWN) {
		w = FONT_WIDTH(scr->txt) * scr->term->min_cols;
		h = FONT_HEIGHT(scr->txt) * scr->term->min_rows;
	} else {
		w = FONT_HEIGHT(scr->txt) * scr->term->min_rows;
		h = FONT_WIDTH(scr->txt) * scr->term->min_cols;
	}
	dw = sw - w;
	dh = sh - h;

	if (dw) {
		if (scr->txt->orientation == OR_NORMAL || scr->txt->orientation == OR_LEFT)
			uterm_display_fill(scr->disp, attr.br, attr.bg, attr.bb, w, 0, dw, sh);
		else
			uterm_display_fill(scr->disp, attr.br, attr.bg, attr.bb, 0, 0, dw, sh);
	}
	if (dh) {
		if (scr->txt->orientation == OR_NORMAL || scr->txt->orientation == OR_RIGHT)
			uterm_display_fill(scr->disp, attr.br, attr.bg, attr.bb, 0, h, sw, dh);
		else
			uterm_display_fill(scr->disp, attr.br, attr.bg, attr.bb, 0, 0, sw, dh);
	}
}

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
	if (!scr->term->pointer.visible)
		return;

	kmscon_text_draw_pointer(scr->txt, scr->term->pointer.x, scr->term->pointer.y);
}

static void do_redraw_screen(struct screen *scr)
{
	struct tsm_screen_attr attr;
	int ret;

	if (!scr->term->awake || !kmscon_session_get_foreground(scr->term->session))
		return;

	scr->pending = false;
	do_clear_margins(scr);

	tsm_vte_get_def_attr(scr->term->vte, &attr);
	kmscon_text_prepare(scr->txt, &attr);
	tsm_screen_draw(scr->term->console, kmscon_text_draw_cb, scr->txt);
	draw_pointer(scr);
	kmscon_text_render(scr->txt);

	ret = uterm_display_swap(scr->disp);
	if (ret) {
		log_warning("cannot swap display [%s] %d", uterm_display_name(scr->disp), ret);
		return;
	}

	scr->swapping = true;
}

static void redraw_screen(struct screen *scr)
{
	if (!scr->term->awake)
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
		if (uterm_display_is_drm(scr->disp))
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

		if (scr->txt->orientation == OR_NORMAL || scr->txt->orientation == OR_UPSIDE_DOWN) {
			sw = uterm_display_get_width(scr->disp);
			sh = uterm_display_get_height(scr->disp);
		} else {
			sw = uterm_display_get_height(scr->disp);
			sh = uterm_display_get_width(scr->disp);
		}
		if (!sw || !sh)
			continue;

		if (sw < max_x)
			max_x = sw;
		if (sh < max_y)
			max_y = sh;
	}
	if (max_x < INT_MAX && max_y < INT_MAX)
		uterm_input_set_pointer_max(term->input, max_x, max_y);
}

static void redraw_all_test(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	if (!term->awake)
		return;

	shl_dlist_for_each(iter, &term->screens)
	{
		scr = shl_dlist_entry(iter, struct screen, list);
		if (uterm_display_is_swapping(scr->disp))
			scr->swapping = true;
		redraw_screen(scr);
	}
}

static void display_event(struct uterm_display *disp, struct uterm_display_event *ev, void *data)
{
	struct screen *scr = data;

	if (ev->action != UTERM_PAGE_FLIP)
		return;

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

static void mouse_event(struct tsm_vte *vte, enum tsm_mouse_track_mode track_mode,
			bool track_pixels, void *data)
{
	struct kmscon_terminal *term = data;

	term->pointer.select = false;
	tsm_screen_selection_reset(term->console);
}

/*
 * Resize terminal
 * We support multiple monitors per terminal. As some software-rendering
 * backends do not support scaling, we always use the smallest cols/rows that are
 * provided so wider displays will have black margins.
 */
static bool terminal_update_size(struct kmscon_terminal *term)
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
	struct kmscon_font *font, *bold_font;
	struct shl_dlist *iter;
	struct screen *ent;

	term->font_attr.bold = false;
	ret = kmscon_font_find(&font, &term->font_attr, term->conf->font_engine);
	if (ret)
		return ret;

	term->font_attr.bold = true;
	ret = kmscon_font_find(&bold_font, &term->font_attr, term->conf->font_engine);
	if (ret) {
		log_warning("cannot create bold font: %d", ret);
		bold_font = font;
		kmscon_font_ref(bold_font);
	}

	kmscon_font_unref(term->bold_font);
	kmscon_font_unref(term->font);
	term->font = font;
	term->bold_font = bold_font;

	term->min_cols = 0;
	term->min_rows = 0;
	shl_dlist_for_each(iter, &term->screens)
	{
		ent = shl_dlist_entry(iter, struct screen, list);

		ret = kmscon_text_set(ent->txt, font, bold_font, ent->disp);
		if (ret)
			log_warning("cannot change text-renderer font: %d", ret);
	}
	terminal_update_size_notify(term);
	return 0;
}

static void rotate_cw_screen(struct screen *scr)
{
	unsigned int orientation = kmscon_text_get_orientation(scr->txt);
	orientation = (orientation + 1) % (OR_LEFT + 1);
	kmscon_text_rotate(scr->txt, orientation);
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

static int add_display(struct kmscon_terminal *term, struct uterm_display *disp)
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

	ret = uterm_display_register_cb(scr->disp, display_event, scr);
	if (ret) {
		log_error("cannot register display callback: %d", ret);
		goto err_free;
	}

	opengl = uterm_display_has_opengl(scr->disp);
	if (term->conf->render_engine)
		be = term->conf->render_engine;
	else if (ret >= 0 && opengl)
		be = "gltex";
	else
		be = "bbulk";

	ret = kmscon_text_new(&scr->txt, be, term->conf->rotate);
	if (ret) {
		log_error("cannot create text-renderer");
		goto err_cb;
	}

	ret = kmscon_text_set(scr->txt, term->font, term->bold_font, scr->disp);
	if (ret) {
		log_error("cannot set text-renderer parameters");
		goto err_text;
	}

	shl_dlist_link(&term->screens, &scr->list);

	log_notice("Display [%s] with backend [%s] text renderer [%s] font engine [%s]\n",
		   uterm_display_name(disp), uterm_display_backend_name(disp), scr->txt->ops->name,
		   term->font->ops->name);

	log_debug("added display %p to terminal %p", disp, term);

	terminal_update_size_notify(term);
	update_pointer_max_all(term);
	uterm_display_ref(scr->disp);
	return 0;

err_text:
	kmscon_text_unref(scr->txt);
err_cb:
	uterm_display_unregister_cb(scr->disp, display_event, scr);
err_free:
	free(scr);
	return ret;
}

static void free_screen(struct screen *scr, bool update)
{
	struct kmscon_terminal *term = scr->term;

	log_debug("destroying terminal screen %p", scr);
	shl_dlist_unlink(&scr->list);
	kmscon_text_unref(scr->txt);
	uterm_display_unregister_cb(scr->disp, display_event, scr);
	uterm_display_unref(scr->disp);
	free(scr);

	if (!update)
		return;

	update_pointer_max_all(term);
	terminal_update_size_notify(term);
}

static void rm_display(struct kmscon_terminal *term, struct uterm_display *disp)
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

static void input_event(struct uterm_input *input, struct uterm_input_key_event *ev, void *data)
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
		if (term->font_attr.points + 1 < term->font_attr.points)
			return;

		++term->font_attr.points;
		if (font_set(term))
			--term->font_attr.points;
		return;
	}
	if (conf_grab_matches(term->conf->grab_zoom_out, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (term->font_attr.points <= 1)
			return;

		--term->font_attr.points;
		if (font_set(term))
			++term->font_attr.points;
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

static void copy_selection(struct kmscon_terminal *term)
{
	if (term->pointer.copy) {
		free(term->pointer.copy);
		term->pointer.copy = NULL;
		term->pointer.copy_len = 0;
	}
	term->pointer.copy_len = tsm_screen_selection_copy(term->console, &term->pointer.copy);
}

static void forward_pointer_event(struct kmscon_terminal *term,
				  struct uterm_input_pointer_event *ev)
{
	unsigned int event;
	unsigned int button;

	button = ev->button;

	switch (ev->event) {
	case UTERM_MOVED:
		event = TSM_MOUSE_EVENT_MOVED;
		break;
	case UTERM_BUTTON:
		if (ev->pressed)
			event = TSM_MOUSE_EVENT_PRESSED;
		else
			event = TSM_MOUSE_EVENT_RELEASED;
		break;
	case UTERM_WHEEL:
		/* Convert wheel events to button 4 (scroll up) or 5 (scroll down) */
		event = TSM_MOUSE_EVENT_PRESSED;
		if (ev->wheel > 0)
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

static void handle_pointer_button(struct kmscon_terminal *term,
				  struct uterm_input_pointer_event *ev)
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
			kmscon_pty_write(term->pty, term->pointer.copy, term->pointer.copy_len);
			tsm_screen_selection_reset(term->console);
		}
	}
}

static void pointer_event(struct uterm_input *input, struct uterm_input_pointer_event *ev,
			  void *data)
{
	struct kmscon_terminal *term = data;

	if (ev->event == UTERM_MOVED) {
		term->pointer.x = ev->pointer_x;
		term->pointer.y = ev->pointer_y;

		coord_to_cell(term, term->pointer.x, term->pointer.y, &term->pointer.posx,
			      &term->pointer.posy);
		term->pointer.visible = true;
	}

	if (tsm_vte_get_mouse_mode(term->vte) != TSM_MOUSE_TRACK_DISABLE &&
	    ev->event != UTERM_SYNC) {
		forward_pointer_event(term, ev);
		return;
	}

	switch (ev->event) {
	default:
		break;
	case UTERM_MOVED:
		if (term->pointer.select)
			update_selection(term->console, term->pointer.posx, term->pointer.posy);
		break;
	case UTERM_BUTTON:
		handle_pointer_button(term, ev);
		break;
	case UTERM_WHEEL:
		tsm_screen_selection_reset(term->console);
		if (ev->wheel > 0)
			tsm_screen_sb_up(term->console, 3);
		else
			tsm_screen_sb_down(term->console, 3);
		break;
	case UTERM_SYNC:
		redraw_all(term);
		break;
	case UTERM_HIDE_TIMEOUT:
		tsm_screen_selection_reset(term->console);
		term->pointer.visible = false;
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

	update_pointer_max_all(term);
	redraw_all(term);
	return 0;
}

static void terminal_close(struct kmscon_terminal *term)
{
	kmscon_pty_close(term->pty);
	term->opened = false;
}

static void terminal_destroy(struct kmscon_terminal *term)
{
	log_debug("free terminal object %p", term);

	terminal_close(term);
	rm_all_screens(term);
	uterm_input_unregister_pointer_cb(term->input, pointer_event, term);
	uterm_input_unregister_key_cb(term->input, input_event, term);
	ev_eloop_rm_fd(term->ptyfd);
	kmscon_pty_unref(term->pty);
	kmscon_font_unref(term->bold_font);
	kmscon_font_unref(term->font);
	tsm_vte_unref(term->vte);
	tsm_screen_unref(term->console);
	uterm_input_unref(term->input);
	ev_eloop_unref(term->eloop);
	free(term);
}

static int session_event(struct kmscon_session *session, struct kmscon_session_event *ev,
			 void *data)
{
	struct kmscon_terminal *term = data;

	switch (ev->type) {
	case KMSCON_SESSION_DISPLAY_NEW:
		add_display(term, ev->disp);
		break;
	case KMSCON_SESSION_DISPLAY_GONE:
		rm_display(term, ev->disp);
		break;
	case KMSCON_SESSION_DISPLAY_REFRESH:
		redraw_all_test(term);
		break;
	case KMSCON_SESSION_ACTIVATE:
		term->awake = true;
		if (!term->opened)
			terminal_open(term);
		redraw_all_test(term);
		break;
	case KMSCON_SESSION_DEACTIVATE:
		term->awake = false;
		break;
	case KMSCON_SESSION_UNREGISTER:
		terminal_destroy(term);
		break;
	}

	return 0;
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

int kmscon_terminal_register(struct kmscon_session **out, struct kmscon_seat *seat,
			     unsigned int vtnr)
{
	struct kmscon_terminal *term;
	int ret;

	if (!out || !seat)
		return -EINVAL;

	term = malloc(sizeof(*term));
	if (!term)
		return -ENOMEM;

	memset(term, 0, sizeof(*term));
	term->ref = 1;
	term->eloop = kmscon_seat_get_eloop(seat);
	term->input = kmscon_seat_get_input(seat);
	shl_dlist_init(&term->screens);

	term->conf_ctx = kmscon_seat_get_conf(seat);
	term->conf = conf_ctx_get_mem(term->conf_ctx);

	strncpy(term->font_attr.name, term->conf->font_name, KMSCON_FONT_MAX_NAME - 1);
	term->font_attr.ppi = term->conf->font_ppi;
	term->font_attr.points = term->conf->font_size;

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

	ret = kmscon_pty_set_conf(term->pty, term->conf->term, "kmscon", term->conf->argv,
				  kmscon_seat_get_name(seat), term->conf->reset_env,
				  term->conf->backspace_delete);
	if (ret)
		goto err_pty;

	ret = ev_eloop_new_fd(term->eloop, &term->ptyfd, kmscon_pty_get_fd(term->pty), EV_READABLE,
			      pty_event, term);
	if (ret)
		goto err_pty;

	ret = uterm_input_register_key_cb(term->input, input_event, term);
	if (ret)
		goto err_ptyfd;

	if (term->conf->mouse) {
		ret = uterm_input_register_pointer_cb(term->input, pointer_event, term);
		if (ret)
			goto err_input;
	}

	ret = kmscon_seat_register_session(seat, &term->session, session_event, term);
	if (ret) {
		log_error("cannot register session for terminal: %d", ret);
		goto err_pointer;
	}

	ev_eloop_ref(term->eloop);
	uterm_input_ref(term->input);
	*out = term->session;
	log_debug("new terminal object %p", term);
	return 0;

err_pointer:
	uterm_input_unregister_pointer_cb(term->input, pointer_event, term);
err_input:
	uterm_input_unregister_key_cb(term->input, input_event, term);
err_ptyfd:
	ev_eloop_rm_fd(term->ptyfd);
err_pty:
	kmscon_pty_unref(term->pty);
err_font:
	kmscon_font_unref(term->bold_font);
	kmscon_font_unref(term->font);
err_vte:
	tsm_vte_unref(term->vte);
err_con:
	tsm_screen_unref(term->console);
err_free:
	free(term);
	return ret;
}
