/*
 * Test of the C bindings to the Rust libtsm (src/tsm/tsm.h).
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "tsm.h"

static struct tsm_vte *vte;
static char written[256];
static size_t written_len;
static char osc[256];
static int bells;
static unsigned int leds;
static int mouse_calls;
static bool mouse_on;

static void on_write(void *data, const char *buf, size_t len)
{
	assert(data == &vte);
	assert(written_len + len <= sizeof(written));
	memcpy(written + written_len, buf, len);
	written_len += len;
}

static void on_osc(void *data, const char *str, size_t len)
{
	assert(strlen(str) == len);
	strcpy(osc, str);
}

static void on_bell(void *data)
{
	bells++;
}

static void on_leds(void *data, unsigned int l)
{
	leds = l;
}

static void on_mouse(void *data, bool enabled, bool pixels)
{
	mouse_calls++;
	mouse_on = enabled;
	/* calling back into the vte from a callback must be allowed */
	tsm_vte_selection_reset(vte);
	assert(tsm_vte_mouse_enabled(vte) == enabled);
}

static const struct tsm_callbacks cbs = {
	.write = on_write,
	.osc = on_osc,
	.bell = on_bell,
	.leds = on_leds,
	.mouse_mode = on_mouse,
};

static void input(const char *s)
{
	tsm_vte_input(vte, s, strlen(s));
}

int main(void)
{
	const struct tsm_cell *cells;
	struct tsm_attr attr;
	unsigned int x, y;
	char *copy = NULL;
	char u8[4];
	int len;

	assert(sizeof(struct tsm_cell) == 12);
	assert(sizeof(struct tsm_attr) == 7);

	assert(tsm_vte_new(&vte, &cbs, &vte) == 0);
	tsm_vte_resize(vte, 20, 5);
	assert(tsm_vte_get_width(vte) == 20);
	assert(tsm_vte_get_height(vte) == 5);

	/* palette */
	assert(tsm_vte_set_palette(vte, "vga") == 0);
	assert(tsm_vte_set_palette(vte, "no-such-palette") == -EINVAL);
	tsm_vte_get_default_attr(vte, &attr);

	/* text and attributes */
	input("\x1b[1;4mab\x1b[m\xe4\xb8\x80z");
	cells = tsm_vte_render(vte);
	assert(cells[0].ch == 'a' && cells[1].ch == 'b');
	assert(cells[0].attr == (TSM_ATTR_BOLD | TSM_ATTR_UNDERLINE));
	assert(cells[2].ch == 0x4e00 && cells[2].attr == 0);
	assert(cells[3].ch == ' '); /* right half of the wide char */
	assert(cells[4].ch == 'z');
	assert(cells[5].ch == ' ' && cells[5].reserved == 0);
	assert(!memcmp(&cells[5].bg, &attr.bg, sizeof(attr.bg)));

	/* cursor */
	assert(tsm_vte_get_cursor(vte, &x, &y));
	assert(x == 5 && y == 0);
	input("\x1b[?25l");
	assert(!tsm_vte_get_cursor(vte, &x, &y));
	input("\x1b[?25h\x1b[6 q");
	assert(tsm_vte_get_cursor_style(vte) == TSM_CURSOR_VBAR_STEADY);
	/* pending wrap: cursor stays inside the screen */
	input("\r\n01234567890123456789");
	assert(tsm_vte_get_cursor(vte, &x, &y));
	assert(x == 19 && y == 1);

	/* replies */
	input("\x1b[c");
	assert(written_len > 0 && written[0] == '\x1b');
	written_len = 0;

	/* callbacks */
	input("\x1b]setBackground\x07");
	assert(!strcmp(osc, "setBackground"));
	input("\a");
	assert(bells == 1);
	input("\x1b[1q");
	assert(leds == TSM_LED_SCROLL_LOCK);

	/* mouse */
	assert(!tsm_vte_mouse_enabled(vte));
	input("\x1b[?1000h");
	assert(mouse_calls == 1 && mouse_on);
	assert(tsm_vte_handle_mouse(vte, 0, 0, 0, 0, TSM_MOUSE_BUTTON_LEFT, TSM_MOUSE_EVENT_PRESSED,
				    0));
	assert(!memcmp(written, "\x1b[M !!", 6));
	written_len = 0;
	input("\x1b[?1000l");
	assert(!mouse_on && !tsm_vte_mouse_enabled(vte));

	/* keyboard */
	assert(tsm_vte_handle_keyboard(vte, 'a', 'a', 0, 'a'));
	assert(written_len == 1 && written[0] == 'a');
	written_len = 0;
	assert(tsm_vte_handle_keyboard(vte, 0xff52 /* Up */, TSM_INVALID, 0, TSM_INVALID));
	assert(written_len == 3);
	written_len = 0;

	/* selection */
	tsm_vte_selection_start(vte, 0, 1);
	tsm_vte_selection_target(vte, 3, 1);
	len = tsm_vte_selection_copy(vte, &copy);
	assert(len == 4 && !strcmp(copy, "0123"));
	free(copy);
	tsm_vte_selection_reset(vte);
	assert(tsm_vte_selection_copy(vte, &copy) < 0);

	/* scrollback: cursor is hidden while scrolled back */
	tsm_vte_set_max_scrollback(vte, 100);
	input("\r\n\n\n\n\n\n");
	tsm_vte_sb_up(vte, 1);
	assert(!tsm_vte_get_cursor(vte, &x, &y));
	tsm_vte_sb_reset(vte);
	assert(tsm_vte_get_cursor(vte, &x, &y));

	/* unicode helpers */
	assert(tsm_char_width('a') == 1);
	assert(tsm_char_width(0x4e00) == 2);
	assert(tsm_char_width(0x0301) == 0);
	assert(tsm_char_width(0xd800) == 0);
	assert(tsm_ucs4_to_utf8(0x20ac, u8) == 3 && !memcmp(u8, "\xe2\x82\xac", 3));
	assert(tsm_ucs4_to_utf8(0x110000, u8) == 0);

	tsm_vte_hard_reset(vte);
	cells = tsm_vte_render(vte);
	assert(cells[0].ch == ' ');

	tsm_vte_free(vte);
	return 0;
}
