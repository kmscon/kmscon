/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * Input Devices
 */

#include <errno.h>
#include <fcntl.h>
#include <libseat.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "eloop.h"
#include "shl_dlist.h"
#include "shl_hook.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "uterm_input.h"
#include "uterm_input_internal.h"

#define LOG_SUBSYSTEM "uterm_input"

/* How many longs are needed to hold \n bits. */
#define NLONGS(n) (((n) + LONG_BIT - 1) / LONG_BIT)

static void input_free_dev(struct uterm_input_dev *dev);
static void input_sleep_dev(struct uterm_input_dev *dev);

static void notify_event(struct uterm_input_dev *dev, uint16_t type, uint16_t code, int32_t value)
{
	switch (type) {
	case EV_KEY:
		if (dev->capabilities & UTERM_DEVICE_HAS_KEYS)
			uxkb_dev_process(dev, value, code);
		pointer_dev_button(dev, code, value);
		break;
	case EV_REL:
		pointer_dev_rel(dev, code, value);
		break;
	case EV_ABS:
		pointer_dev_abs(dev, code, value);
		break;
	case EV_SYN:
		pointer_dev_sync(dev);
		break;
	}
}

static void input_data_dev(struct ev_fd *fd, int mask, void *data)
{
	struct uterm_input_dev *dev = data;
	struct input_event ev[16];
	ssize_t len, n;
	int i;

	if (mask & (EV_HUP | EV_ERR)) {
		log_debug("EOF on %s", dev->node);
		input_sleep_dev(dev);
		return;
	}

	len = sizeof(ev);
	while (len == sizeof(ev)) {
		len = read(dev->rfd, &ev, sizeof(ev));
		if (len < 0) {
			if (errno == EWOULDBLOCK)
				break;
			log_warning("reading from %s failed (%d): %m", dev->node, errno);
			input_sleep_dev(dev);
		} else if (len == 0) {
			log_debug("EOF on %s", dev->node);
			input_sleep_dev(dev);
		} else if (len % sizeof(*ev)) {
			log_warn("invalid input_event on %s", dev->node);
		} else {
			n = len / sizeof(*ev);
			for (i = 0; i < n; i++)
				notify_event(dev, ev[i].type, ev[i].code, ev[i].value);
		}
	}
}

static int input_wake_up_dev(struct uterm_input_dev *dev)
{
	int ret, fd;

	if (dev->rfd >= 0)
		return 0;

	if (dev->input->libseat) {
		dev->device_id = libseat_open_device(dev->input->libseat, dev->node, &fd);
		if (dev->device_id < 0) {
			log_warning("cannot open device %s via libseat (%d): %m", dev->node, errno);
			return -EFAULT;
		}
		dev->rfd = fd;
	} else {
		dev->rfd = open(dev->node, O_CLOEXEC | O_NONBLOCK | O_RDWR);
		if (dev->rfd < 0) {
			log_warning("cannot open device %s (%d): %m", dev->node, errno);
			return -EFAULT;
		}
		dev->device_id = -1;
	}

	if (dev->capabilities & UTERM_DEVICE_HAS_KEYS)
		uxkb_dev_wake_up(dev);

	ret = ev_eloop_new_fd(dev->input->eloop, &dev->fd, dev->rfd, EV_READABLE, input_data_dev,
			      dev);
	if (ret) {
		if (dev->input->libseat && dev->device_id >= 0)
			libseat_close_device(dev->input->libseat, dev->device_id);
		close(dev->rfd);
		dev->rfd = -1;
		dev->device_id = -1;
		return ret;
	}

	return 0;
}

static void input_sleep_dev(struct uterm_input_dev *dev)
{
	if (dev->rfd < 0)
		return;

	dev->repeating = false;
	ev_timer_update(dev->repeat_timer, NULL);
	ev_eloop_rm_fd(dev->fd);
	dev->fd = NULL;
	if (dev->device_id >= 0)
		libseat_close_device(dev->input->libseat, dev->device_id);
	close(dev->rfd);
	dev->rfd = -1;
	dev->device_id = -1;
}

static int input_init_keyboard(struct uterm_input_dev *dev)
{
	dev->num_syms = 1;
	dev->event.keysyms = malloc(sizeof(uint32_t) * dev->num_syms);
	if (!dev->event.keysyms)
		return -1;
	dev->event.codepoints = malloc(sizeof(uint32_t) * dev->num_syms);
	if (!dev->event.codepoints)
		goto err_syms;
	dev->repeat_event.keysyms = malloc(sizeof(uint32_t) * dev->num_syms);
	if (!dev->repeat_event.keysyms)
		goto err_codepoints;
	dev->repeat_event.codepoints = malloc(sizeof(uint32_t) * dev->num_syms);
	if (!dev->repeat_event.codepoints)
		goto err_rsyms;

	if (uxkb_dev_init(dev))
		goto err_rcodepoints;

	return 0;

err_rcodepoints:
	free(dev->repeat_event.codepoints);
err_rsyms:
	free(dev->repeat_event.keysyms);
err_codepoints:
	free(dev->event.codepoints);
err_syms:
	free(dev->event.keysyms);
	return -1;
}

static void input_exit_keyboard(struct uterm_input_dev *dev)
{
	uxkb_dev_destroy(dev);
	free(dev->repeat_event.codepoints);
	free(dev->repeat_event.keysyms);
	free(dev->event.codepoints);
	free(dev->event.keysyms);
}

static int input_init_abs(struct uterm_input_dev *dev, int fd)
{
	struct input_absinfo info;
	int ret;

	ret = ioctl(fd, EVIOCGABS(ABS_X), &info);
	if (ret < 0)
		return ret;

	dev->pointer.min_x = info.minimum;
	dev->pointer.max_x = info.maximum;

	ret = ioctl(fd, EVIOCGABS(ABS_Y), &info);
	if (ret < 0)
		return ret;

	dev->pointer.min_y = info.minimum;
	dev->pointer.max_y = info.maximum;

	log_debug("ABSX min %d max %d ABSY min %d max %d\n", dev->pointer.min_x, dev->pointer.max_x,
		  dev->pointer.min_y, dev->pointer.max_y);
	return 0;
}

static const char *pointer_kind_name(enum pointer_kind kind)
{
	switch (kind) {
	default:
	case POINTER_NONE:
		return "";
	case POINTER_TOUCHPAD:
		return "Touchpad";
	case POINTER_MOUSE:
		return "Mouse";
	case POINTER_TOUCHSCREEN:
		return "Touchscreen";
	case POINTER_VMOUSE:
		return "Virtual mouse";
	}
}

/*
 * Create a new input device from a probing fd.
 * The fd is NOT consumed here - the caller manages it. The device starts
 * with rfd=-1 and will be woken up separately via input_wake_up_dev().
 */
static struct uterm_input_dev *input_new_dev(struct uterm_input *input, const char *node,
					     int probe_fd, unsigned int capabilities)
{
	struct uterm_input_dev *dev;
	char name[64] = {0};
	int ret;

	dev = malloc(sizeof(*dev));
	if (!dev)
		return NULL;
	memset(dev, 0, sizeof(*dev));
	dev->input = input;
	dev->rfd = -1;
	dev->device_id = -1;
	dev->capabilities = capabilities;
	dev->pointer.kind = POINTER_NONE;
	dev->pointer.pressed_button = BUTTON_NONE;

	dev->node = strdup(node);
	if (!dev->node)
		goto err_free;

	if (dev->capabilities & UTERM_DEVICE_HAS_KEYS) {
		ret = input_init_keyboard(dev);
		if (ret)
			goto err_node;
	}
	if (dev->capabilities & UTERM_DEVICE_HAS_ABS) {
		ret = input_init_abs(dev, probe_fd);
		if (ret)
			goto err_kbd;
		if (dev->capabilities & UTERM_DEVICE_HAS_TOUCH) {
			if (dev->capabilities & UTERM_DEVICE_HAS_DIRECT)
				dev->pointer.kind = POINTER_TOUCHSCREEN;
			else
				dev->pointer.kind = POINTER_TOUCHPAD;
		} else
			dev->pointer.kind = POINTER_VMOUSE;
	}
	if (dev->capabilities & UTERM_DEVICE_HAS_REL)
		dev->pointer.kind = POINTER_MOUSE;

	ioctl(probe_fd, EVIOCGNAME(sizeof(name) - 1), name);
	log_info("Using device %s [%s] as %s%s\n", node, name,
		 (dev->capabilities & UTERM_DEVICE_HAS_KEYS) ? "keyboard" : "",
		 pointer_kind_name(dev->pointer.kind));
	shl_dlist_link(&input->devices, &dev->list);
	return dev;

err_kbd:
	if (dev->capabilities & UTERM_DEVICE_HAS_KEYS)
		input_exit_keyboard(dev);
err_node:
	free(dev->node);
err_free:
	free(dev);
	return NULL;
}

static void input_free_dev(struct uterm_input_dev *dev)
{
	log_debug("free device %s", dev->node);
	input_sleep_dev(dev);
	shl_dlist_unlink(&dev->list);
	if (dev->capabilities & UTERM_DEVICE_HAS_KEYS)
		input_exit_keyboard(dev);
	free(dev->node);
	free(dev);
}

static void hide_pointer_timer(struct ev_timer *timer, uint64_t num, void *data)
{
	struct uterm_input *input = data;
	struct uterm_input_pointer_event pev;

	pev.event = UTERM_HIDE_TIMEOUT;

	shl_hook_call(input->pointer_hook, input, &pev);
}

SHL_EXPORT
int uterm_input_new(struct uterm_input **out, struct ev_eloop *eloop, struct libseat *libseat,
		    const char *model, const char *layout, const char *variant, const char *options,
		    const char *locale, const char *keymap, const char *compose_file,
		    size_t compose_file_len, unsigned int repeat_delay, unsigned int repeat_rate)
{
	struct uterm_input *input;
	int ret;

	if (!out || !eloop)
		return -EINVAL;

	if (!repeat_delay)
		repeat_delay = 250;
	if (repeat_delay >= 1000)
		repeat_delay = 999;
	if (!repeat_rate)
		repeat_rate = 50;
	if (repeat_rate >= 1000)
		repeat_rate = 999;

	input = malloc(sizeof(*input));
	if (!input)
		return -ENOMEM;
	memset(input, 0, sizeof(*input));
	input->ref = 1;
	input->eloop = eloop;
	input->libseat = libseat;
	input->repeat_delay = repeat_delay;
	input->repeat_rate = repeat_rate;
	shl_dlist_init(&input->devices);
	shl_dlist_init(&input->pending_devices);

	ret = shl_hook_new(&input->key_hook);
	if (ret)
		goto err_free;

	ret = shl_hook_new(&input->pointer_hook);
	if (ret)
		goto err_hook;

	ret = ev_eloop_new_timer(input->eloop, &input->hide_pointer, NULL, hide_pointer_timer,
				 input);
	if (ret)
		goto err_hook_pointer;

	/* xkbcommon won't use the XKB_DEFAULT_OPTIONS environment
	 * variable if options is an empty string.
	 * So if all variables are empty, use NULL instead.
	 */
	if (model && *model == 0 && layout && *layout == 0 && variant && *variant == 0 && options &&
	    *options == 0) {
		model = NULL;
		layout = NULL;
		variant = NULL;
		options = NULL;
	}

	ret = uxkb_desc_init(input, model, layout, variant, options, locale, keymap, compose_file,
			     compose_file_len);
	if (ret)
		goto err_hide_timer;

	log_debug("new object %p", input);
	ev_eloop_ref(input->eloop);
	*out = input;
	return 0;

err_hide_timer:
	ev_eloop_rm_timer(input->hide_pointer);

err_hook_pointer:
	shl_hook_free(input->pointer_hook);

err_hook:
	shl_hook_free(input->key_hook);

err_free:
	free(input);
	return ret;
}

SHL_EXPORT
void uterm_input_ref(struct uterm_input *input)
{
	if (!input || !input->ref)
		return;

	++input->ref;
}

SHL_EXPORT
void uterm_input_unref(struct uterm_input *input)
{
	struct uterm_input_dev *dev;
	struct uterm_input_pending *pend;

	if (!input || !input->ref || --input->ref)
		return;

	log_debug("free object %p", input);

	while (input->devices.next != &input->devices) {
		dev = shl_dlist_entry(input->devices.next, struct uterm_input_dev, list);
		input_free_dev(dev);
	}

	while (input->pending_devices.next != &input->pending_devices) {
		pend = shl_dlist_entry(input->pending_devices.next, struct uterm_input_pending,
				       list);
		shl_dlist_unlink(&pend->list);
		free(pend->node);
		free(pend);
	}

	uxkb_desc_destroy(input);
	shl_hook_free(input->key_hook);
	shl_hook_free(input->pointer_hook);
	ev_eloop_rm_timer(input->hide_pointer);
	ev_eloop_unref(input->eloop);
	free(input);
}

/*
 * See if the device has anything useful to offer.
 * We go over the possible capabilities and return a mask of enum
 * uterm_input_device_capability's.
 */
static unsigned int probe_device_capabilities(int fd, struct uterm_input *input, const char *name)
{
	int i, ret;
	unsigned int capabilities = 0;
	unsigned long evbits[NLONGS(EV_CNT)] = {0};
	unsigned long keybits[NLONGS(KEY_CNT)] = {0};
	unsigned long relbits[NLONGS(REL_CNT)] = {0};
	unsigned long absbits[NLONGS(ABS_CNT)] = {0};
	unsigned long props[NLONGS(INPUT_PROP_CNT)] = {0};

	/* Which types of input events the device supports. */
	ret = ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
	if (ret == -1)
		goto err_ioctl;

	/* Device supports keys/buttons. */
	if (input_bit_is_set(evbits, EV_KEY)) {
		ret = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
		if (ret == -1)
			goto err_ioctl;

		/*
		 * If the device support any of the normal keyboard keys, we
		 * take it. Even if the keys are not ordinary they can be
		 * mapped to anything by the keyboard backend.
		 */
		for (i = KEY_RESERVED; i <= KEY_MIN_INTERESTING; i++) {
			if (input_bit_is_set(keybits, i)) {
				capabilities |= UTERM_DEVICE_HAS_KEYS;
				break;
			}
		}
		if (input_bit_is_set(keybits, BTN_LEFT))
			capabilities |= UTERM_DEVICE_HAS_MOUSE_BTN;
		if (input_bit_is_set(keybits, BTN_TOUCH))
			capabilities |= UTERM_DEVICE_HAS_TOUCH;
	}

	if (input_bit_is_set(evbits, EV_SYN) && input_bit_is_set(evbits, EV_REL)) {
		ret = ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits);
		if (ret < 0)
			goto err_ioctl;
		if (input_bit_is_set(relbits, REL_X) && input_bit_is_set(relbits, REL_Y))
			capabilities |= UTERM_DEVICE_HAS_REL;
		if (input_bit_is_set(relbits, REL_WHEEL))
			capabilities |= UTERM_DEVICE_HAS_WHEEL;
	}

	if (input_bit_is_set(evbits, EV_SYN) && input_bit_is_set(evbits, EV_ABS)) {
		ret = ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
		if (ret < 0)
			goto err_ioctl;
		if (input_bit_is_set(absbits, ABS_X) && input_bit_is_set(absbits, ABS_Y))
			capabilities |= UTERM_DEVICE_HAS_ABS;

		ret = ioctl(fd, EVIOCGPROP(sizeof(props)), props);
		if (ret < 0)
			goto err_ioctl;
		if (input_bit_is_set(props, INPUT_PROP_DIRECT))
			capabilities |= UTERM_DEVICE_HAS_DIRECT;
	}

	if (input_bit_is_set(evbits, EV_LED))
		capabilities |= UTERM_DEVICE_HAS_LEDS;

	return capabilities;

err_ioctl:
	log_warn("cannot probe capabilities of device %s (%d): %m", name, errno);
	return 0;
}

#define HAS_ALL(caps, flags) (((caps) & (flags)) == (flags))

/*
 * Probe a device via an already-opened fd and add it to the active device list
 * if it has useful capabilities. Returns true if the device was added, false
 * if it was rejected.
 */
static bool input_probe_and_add(struct uterm_input *input, const char *node, int fd, bool mouse)
{
	unsigned int capabilities;
	char name[64] = {0};

	ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
	capabilities = probe_device_capabilities(fd, input, name);

	if (HAS_ALL(capabilities, UTERM_DEVICE_HAS_KEYS))
		return !!input_new_dev(input, node, fd, capabilities);

	if (HAS_ALL(capabilities, UTERM_DEVICE_HAS_REL | UTERM_DEVICE_HAS_MOUSE_BTN) ||
	    HAS_ALL(capabilities, UTERM_DEVICE_HAS_ABS | UTERM_DEVICE_HAS_TOUCH) ||
	    HAS_ALL(capabilities, UTERM_DEVICE_HAS_ABS | UTERM_DEVICE_HAS_MOUSE_BTN)) {
		if (mouse)
			return !!input_new_dev(input, node, fd, capabilities);
		log_debug("ignoring pointer device %s [%s]", node, name);
	} else {
		log_debug("ignoring non-useful device %s [%s]", node, name);
	}

	return false;
}

/*
 * Open an input device, probe it, and if useful, add it to the active list
 * and register it with the event loop. The device is opened via libseat if
 * available, otherwise directly.
 */
static void input_open_probe_activate(struct uterm_input *input, const char *node, bool mouse)
{
	int fd, device_id = -1, ret;
	struct uterm_input_dev *dev;

	if (input->libseat) {
		device_id = libseat_open_device(input->libseat, node, &fd);
		if (device_id < 0) {
			log_warning("cannot open device %s via libseat: %m", node);
			return;
		}
	} else {
		fd = open(node, O_NONBLOCK | O_CLOEXEC | O_RDWR);
		if (fd < 0) {
			log_warning("cannot open device %s: %m", node);
			return;
		}
	}

	if (!input_probe_and_add(input, node, fd, mouse))
		goto err_close;

	/* Device was added at the head of the active list. Wire up its fd. */
	dev = shl_dlist_entry(input->devices.next, struct uterm_input_dev, list);
	dev->rfd = fd;
	dev->device_id = device_id;
	if (dev->capabilities & UTERM_DEVICE_HAS_KEYS)
		uxkb_dev_wake_up(dev);

	ret = ev_eloop_new_fd(input->eloop, &dev->fd, dev->rfd, EV_READABLE, input_data_dev, dev);
	if (ret) {
		input_free_dev(dev);
		goto err_close;
	}
	return;

err_close:
	if (input->libseat && device_id >= 0)
		libseat_close_device(input->libseat, device_id);
	close(fd);
}

SHL_EXPORT
void uterm_input_add_dev(struct uterm_input *input, const char *node, bool mouse)
{
	struct uterm_input_pending *pend;

	if (!input || !node)
		return;

	pend = malloc(sizeof(*pend));
	if (!pend)
		return;
	memset(pend, 0, sizeof(*pend));
	pend->node = strdup(node);
	if (!pend->node) {
		free(pend);
		return;
	}
	pend->mouse = mouse;
	shl_dlist_link_tail(&input->pending_devices, &pend->list);

	/* If already awake, probe and activate immediately */
	if (input->awake > 0) {
		shl_dlist_unlink(&pend->list);
		input_open_probe_activate(input, node, mouse);
		free(pend->node);
		free(pend);
	}
}

SHL_EXPORT
void uterm_input_remove_dev(struct uterm_input *input, const char *node)
{
	struct shl_dlist *iter;
	struct uterm_input_dev *dev;
	struct uterm_input_pending *pend;

	if (!input || !node)
		return;

	/* Check active devices */
	shl_dlist_for_each(iter, &input->devices)
	{
		dev = shl_dlist_entry(iter, struct uterm_input_dev, list);
		if (!strcmp(dev->node, node)) {
			input_free_dev(dev);
			return;
		}
	}

	/* Check pending devices */
	shl_dlist_for_each(iter, &input->pending_devices)
	{
		pend = shl_dlist_entry(iter, struct uterm_input_pending, list);
		if (!strcmp(pend->node, node)) {
			shl_dlist_unlink(&pend->list);
			free(pend->node);
			free(pend);
			return;
		}
	}
}

SHL_EXPORT
int uterm_input_register_key_cb(struct uterm_input *input, uterm_input_key_cb cb, void *data)
{
	if (!input || !cb)
		return -EINVAL;

	return shl_hook_add_cast(input->key_hook, cb, data, false);
}

SHL_EXPORT
void uterm_input_unregister_key_cb(struct uterm_input *input, uterm_input_key_cb cb, void *data)
{
	if (!input || !cb)
		return;

	shl_hook_rm_cast(input->key_hook, cb, data);
}

SHL_EXPORT
int uterm_input_register_pointer_cb(struct uterm_input *input, uterm_input_pointer_cb cb,
				    void *data)
{
	if (!input || !cb)
		return -EINVAL;

	return shl_hook_add_cast(input->pointer_hook, cb, data, false);
}

SHL_EXPORT
void uterm_input_unregister_pointer_cb(struct uterm_input *input, uterm_input_pointer_cb cb,
				       void *data)
{
	if (!input || !cb)
		return;

	shl_hook_rm_cast(input->pointer_hook, cb, data);
}

SHL_EXPORT
void uterm_input_sleep(struct uterm_input *input)
{
	struct shl_dlist *iter;
	struct uterm_input_dev *dev;

	if (!input)
		return;

	--input->awake;
	if (input->awake != 0)
		return;

	log_debug("going to sleep");

	shl_dlist_for_each(iter, &input->devices)
	{
		dev = shl_dlist_entry(iter, struct uterm_input_dev, list);
		input_sleep_dev(dev);
	}
}

SHL_EXPORT
void uterm_input_wake_up(struct uterm_input *input)
{
	struct shl_dlist *iter, *tmp;
	struct uterm_input_dev *dev;
	struct uterm_input_pending *pend;
	int ret;

	if (!input)
		return;

	++input->awake;
	if (input->awake != 1)
		return;

	log_debug("waking up");

	/* Process pending devices: probe and add */
	shl_dlist_for_each_safe(iter, tmp, &input->pending_devices)
	{
		pend = shl_dlist_entry(iter, struct uterm_input_pending, list);
		shl_dlist_unlink(&pend->list);
		input_open_probe_activate(input, pend->node, pend->mouse);
		free(pend->node);
		free(pend);
	}

	/* Wake up already-probed devices */
	shl_dlist_for_each_safe(iter, tmp, &input->devices)
	{
		dev = shl_dlist_entry(iter, struct uterm_input_dev, list);
		ret = input_wake_up_dev(dev);
		if (ret)
			input_free_dev(dev);
	}
}

SHL_EXPORT
bool uterm_input_is_awake(struct uterm_input *input)
{
	if (!input)
		return false;

	return input->awake > 0;
}

SHL_EXPORT
void uterm_input_set_pointer_max(struct uterm_input *input, unsigned int max_x, unsigned int max_y)
{
	if (!input)
		return;

	input->pointer_max_x = max_x;
	input->pointer_max_y = max_y;
}

SHL_EXPORT
void uterm_input_set_leds(struct uterm_input *input, unsigned int scroll_lock,
			  unsigned int num_lock, unsigned int caps_lock)
{
	struct shl_dlist *iter;
	struct uterm_input_dev *dev;

	if (!input)
		return;

	shl_dlist_for_each(iter, &input->devices)
	{
		dev = shl_dlist_entry(iter, struct uterm_input_dev, list);
		uxkb_dev_set_leds(dev, scroll_lock, num_lock, caps_lock);
	}
}
