/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
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

/* Internal definitions */

#ifndef UTERM_INPUT_INTERNAL_H
#define UTERM_INPUT_INTERNAL_H

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "eloop.h"
#include "shl_dlist.h"
#include "shl_llog.h"
#include "shl_misc.h"
#include "uterm_input.h"

enum uterm_input_device_capability {
	UTERM_DEVICE_HAS_KEYS = (1 << 0),
	UTERM_DEVICE_HAS_LEDS = (1 << 1),
	UTERM_DEVICE_HAS_REL = (1 << 2),
	UTERM_DEVICE_HAS_ABS = (1 << 3),
	UTERM_DEVICE_HAS_MOUSE_BTN = (1 << 4),
	UTERM_DEVICE_HAS_TOUCH = (1 << 5),
	UTERM_DEVICE_HAS_WHEEL = (1 << 6),
	UTERM_DEVICE_HAS_DIRECT = (1 << 7),
};

enum pointer_kind {
	POINTER_NONE,
	POINTER_MOUSE,
	POINTER_TOUCHPAD,
	POINTER_TOUCHSCREEN,
	POINTER_VMOUSE,
};

/* Button state for pressed_button field */
#define BUTTON_NONE 255

struct uterm_input_pointer {
	/* For pointers (mouse/trackpad/trackpoint/touchscreen) */
	enum pointer_kind kind;
	enum uterm_input_pointer_type action;
	struct timespec last_click;
	int32_t x;
	int32_t y;

	bool touchpaddown;
	int32_t off_x;
	int32_t off_y;

	int32_t min_x;
	int32_t max_x;
	int32_t min_y;
	int32_t max_y;

	/* Track which button is currently pressed (BUTTON_NONE=none, 0=left, 1=right, 2=middle) */
	uint8_t pressed_button;
};

struct uterm_input_dev {
	struct shl_dlist list;
	struct uterm_input *input;

	unsigned int capabilities;
	int rfd;
	char *node;
	struct ev_fd *fd;

	/* For keyboards */
	struct xkb_state *state;
	struct xkb_compose_state *compose_state;
	/* Used in sleep/wake up to store the key's pressed/released state. */
	char key_state_bits[SHL_DIV_ROUND_UP(KEY_CNT, CHAR_BIT)];

	unsigned int num_syms;
	struct uterm_input_key_event event;
	struct uterm_input_key_event repeat_event;

	bool repeating;
	struct ev_timer *repeat_timer;

	struct uterm_input_pointer pointer;
};

struct uterm_input {
	unsigned long ref;
	llog_submit_t llog;
	void *llog_data;
	struct ev_eloop *eloop;
	int awake;
	unsigned int repeat_rate;
	unsigned int repeat_delay;

	struct shl_hook *key_hook;
	struct xkb_context *ctx;
	struct xkb_keymap *keymap;
	struct xkb_compose_table *compose_table;

	struct shl_hook *pointer_hook;
	int32_t pointer_max_x;
	int32_t pointer_max_y;
	struct ev_timer *hide_pointer;

	struct shl_dlist devices;
};

static inline bool input_bit_is_set(const unsigned long *array, int bit)
{
	return !!(array[bit / LONG_BIT] & (1UL << (bit % LONG_BIT)));
}

int uxkb_desc_init(struct uterm_input *input, const char *model, const char *layout,
		   const char *variant, const char *options, const char *locale, const char *keymap,
		   const char *compose_file, size_t compose_file_len);
void uxkb_desc_destroy(struct uterm_input *input);

int uxkb_dev_init(struct uterm_input_dev *dev);
void uxkb_dev_destroy(struct uterm_input_dev *dev);
int uxkb_dev_process(struct uterm_input_dev *dev, uint16_t key_state, uint16_t code);
void uxkb_dev_sleep(struct uterm_input_dev *dev);
void uxkb_dev_wake_up(struct uterm_input_dev *dev);

void pointer_dev_rel(struct uterm_input_dev *dev, uint16_t code, int32_t value);
void pointer_dev_abs(struct uterm_input_dev *dev, uint16_t code, int32_t value);
void pointer_dev_button(struct uterm_input_dev *dev, uint16_t code, int32_t value);
void pointer_dev_sync(struct uterm_input_dev *dev);

#endif /* UTERM_INPUT_INTERNAL_H */
