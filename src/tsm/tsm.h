/*
 * tsm - C interface to the Rust libtsm terminal-emulator state machine
 *
 * SPDX-License-Identifier: MIT
 *
 * The implementation lives in tsm.rs, which wraps the libtsm crate bundled in
 * libtsm/. It is built by cargo as a static library and linked into kmscon.
 *
 * A single opaque struct tsm_vte owns both the parser and the screen.
 */

#ifndef TSM_H
#define TSM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "no unicode / no keysym" value for tsm_vte_handle_keyboard() */
#define TSM_INVALID 0xffffffff

/* palette entries, in the order expected by tsm_vte_set_custom_palette() */
enum tsm_color_index {
	TSM_COLOR_BLACK,
	TSM_COLOR_RED,
	TSM_COLOR_GREEN,
	TSM_COLOR_YELLOW,
	TSM_COLOR_BLUE,
	TSM_COLOR_MAGENTA,
	TSM_COLOR_CYAN,
	TSM_COLOR_LIGHT_GREY,
	TSM_COLOR_DARK_GREY,
	TSM_COLOR_LIGHT_RED,
	TSM_COLOR_LIGHT_GREEN,
	TSM_COLOR_LIGHT_YELLOW,
	TSM_COLOR_LIGHT_BLUE,
	TSM_COLOR_LIGHT_MAGENTA,
	TSM_COLOR_LIGHT_CYAN,
	TSM_COLOR_WHITE,
	TSM_COLOR_FOREGROUND,
	TSM_COLOR_BACKGROUND,
	TSM_COLOR_NUM
};

/* cell attribute flags (struct tsm_cell.attr, struct tsm_attr.flags) */
#define TSM_ATTR_BOLD 0x01
#define TSM_ATTR_ITALIC 0x02
#define TSM_ATTR_UNDERLINE 0x04
#define TSM_ATTR_INVERSE 0x08
#define TSM_ATTR_PROTECT 0x10
#define TSM_ATTR_BLINK 0x20
#define TSM_ATTR_DIM 0x40

/* keyboard LEDs, passed to the leds callback */
#define TSM_LED_SCROLL_LOCK 0x01
#define TSM_LED_NUM_LOCK 0x02
#define TSM_LED_CAPS_LOCK 0x04

/* keyboard modifiers for tsm_vte_handle_keyboard() */
#define TSM_SHIFT_MASK 0x01
#define TSM_LOCK_MASK 0x02
#define TSM_CONTROL_MASK 0x04
#define TSM_ALT_MASK 0x08
#define TSM_LOGO_MASK 0x10

/* mouse events and buttons for tsm_vte_handle_mouse() */
#define TSM_MOUSE_EVENT_PRESSED 0x01
#define TSM_MOUSE_EVENT_RELEASED 0x02
#define TSM_MOUSE_EVENT_MOVED 0x04

#define TSM_MOUSE_BUTTON_LEFT 0
#define TSM_MOUSE_BUTTON_MIDDLE 1
#define TSM_MOUSE_BUTTON_RIGHT 2
#define TSM_MOUSE_BUTTON_WHEEL_UP 4
#define TSM_MOUSE_BUTTON_WHEEL_DOWN 5
#define TSM_MOUSE_BUTTON_DRAG 32 /* add to a button for moves while it is held */

/* cursor style, as set by DECSCUSR */
enum tsm_cursor_style {
	TSM_CURSOR_DEFAULT = 0,
	TSM_CURSOR_BLOCK_BLINK = 1,
	TSM_CURSOR_BLOCK_STEADY = 2,
	TSM_CURSOR_UNDERLINE_BLINK = 3,
	TSM_CURSOR_UNDERLINE_STEADY = 4,
	TSM_CURSOR_VBAR_BLINK = 5,
	TSM_CURSOR_VBAR_STEADY = 6,
};

struct tsm_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/* default attribute of the terminal (colors resolved from the palette) */
struct tsm_attr {
	struct tsm_color fg;
	struct tsm_color bg;
	uint8_t flags; /* TSM_ATTR_* */
};

/*
 * A rendered cell. Colors are final (inverse, selection and dim are applied).
 * Empty cells and the right half of wide characters have ch == ' '.
 * attr only contains TSM_ATTR_BOLD, ITALIC, UNDERLINE and BLINK.
 */
struct tsm_cell {
	uint32_t ch;
	struct tsm_color fg;
	struct tsm_color bg;
	uint8_t attr;
	uint8_t reserved; /* always 0, so cells can be compared with memcmp */
};

/*
 * Callbacks, all optional. write is called synchronously; the others are
 * called after the triggering tsm_vte_* call has finished its work, so they
 * may call back into the tsm_vte_* API.
 */
struct tsm_callbacks {
	/* data to send to the application (replies, keyboard, mouse) */
	void (*write)(void *data, const char *buf, size_t len);
	/* OSC string, without "ESC ]" and terminator, NUL-terminated */
	void (*osc)(void *data, const char *str, size_t len);
	void (*bell)(void *data);
	/* TSM_LED_* mask */
	void (*leds)(void *data, unsigned int leds);
	/* the application changed mouse reporting */
	void (*mouse_mode)(void *data, bool enabled, bool pixels);
};

struct tsm_vte;

int tsm_vte_new(struct tsm_vte **out, const struct tsm_callbacks *cb, void *data);
void tsm_vte_free(struct tsm_vte *vte);

void tsm_vte_input(struct tsm_vte *vte, const char *buf, size_t len);
void tsm_vte_paste(struct tsm_vte *vte, const char *buf, size_t len);
void tsm_vte_hard_reset(struct tsm_vte *vte);
void tsm_vte_set_backspace_sends_delete(struct tsm_vte *vte, bool enable);

/* set a predefined palette by name, returns -EINVAL if unknown */
int tsm_vte_set_palette(struct tsm_vte *vte, const char *name);
/* set a palette from TSM_COLOR_NUM rgb entries */
int tsm_vte_set_custom_palette(struct tsm_vte *vte, const uint8_t (*rgb)[3]);
void tsm_vte_get_default_attr(struct tsm_vte *vte, struct tsm_attr *out);

/* returns true if the key produced output */
bool tsm_vte_handle_keyboard(struct tsm_vte *vte, uint32_t keysym, uint32_t ascii,
			     unsigned int mods, uint32_t unicode);
/* returns true if the event was reported to the application */
bool tsm_vte_handle_mouse(struct tsm_vte *vte, unsigned int cell_x, unsigned int cell_y,
			  unsigned int pixel_x, unsigned int pixel_y, uint8_t button,
			  unsigned int event, uint8_t mods);
/* true if the application wants mouse events */
bool tsm_vte_mouse_enabled(struct tsm_vte *vte);

/* screen */
void tsm_vte_resize(struct tsm_vte *vte, unsigned int cols, unsigned int rows);
unsigned int tsm_vte_get_width(struct tsm_vte *vte);
unsigned int tsm_vte_get_height(struct tsm_vte *vte);
void tsm_vte_set_max_scrollback(struct tsm_vte *vte, unsigned int max);
void tsm_vte_sb_up(struct tsm_vte *vte, unsigned int num);
void tsm_vte_sb_down(struct tsm_vte *vte, unsigned int num);
void tsm_vte_sb_page_up(struct tsm_vte *vte, unsigned int num);
void tsm_vte_sb_page_down(struct tsm_vte *vte, unsigned int num);
void tsm_vte_sb_reset(struct tsm_vte *vte);

/* width * height cells, row-major; valid until the next call on vte */
const struct tsm_cell *tsm_vte_render(struct tsm_vte *vte);
/* cursor position, always inside the screen; returns false if hidden */
bool tsm_vte_get_cursor(struct tsm_vte *vte, unsigned int *x, unsigned int *y);
enum tsm_cursor_style tsm_vte_get_cursor_style(struct tsm_vte *vte);
unsigned int tsm_vte_get_cursor_blink_rate_ms(struct tsm_vte *vte);

/* selection, in visible cell coordinates */
void tsm_vte_selection_reset(struct tsm_vte *vte);
void tsm_vte_selection_start(struct tsm_vte *vte, unsigned int x, unsigned int y);
void tsm_vte_selection_target(struct tsm_vte *vte, unsigned int x, unsigned int y);
void tsm_vte_selection_word(struct tsm_vte *vte, unsigned int x, unsigned int y);
/* *out is malloc'd and NUL-terminated; returns its length or a negative errno */
int tsm_vte_selection_copy(struct tsm_vte *vte, char **out);

/* unicode helpers */
/* display width of a codepoint: 0 (control, combining, invalid), 1 or 2 */
unsigned int tsm_char_width(uint32_t ucs4);
/* encode into out[4], returns the number of bytes (0 if invalid) */
size_t tsm_ucs4_to_utf8(uint32_t ucs4, char *out);

#ifdef __cplusplus
}
#endif

#endif /* TSM_H */
