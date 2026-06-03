/*
 * Seats
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

/*
 * Seats
 * A seat is a single session that is self-hosting and provides all the
 * interaction for a single logged-in user.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>
#include "conf.h"
#include "input/input.h"
#include "kmscon_conf.h"
#include "kmscon_seat.h"
#include "kmscon_terminal.h"
#include "shl/dlist.h"
#include "shl/eloop.h"
#include "shl/log.h"
#include "uterm_monitor.h"
#include "uterm_vt.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "seat"

struct kmscon_session {
	struct shl_dlist list;
	struct kmscon_seat *seat;

	bool foreground;

	struct ev_timer *timer;
	struct kmscon_terminal *term;
};

struct kmscon_display {
	struct shl_dlist list;
	struct kmscon_seat *seat;
	struct display *disp;
	bool activated;
};

struct kmscon_video {
	struct shl_dlist list;
	struct kmscon_seat *seat;
	struct video *video;
	struct uterm_monitor_dev *udev;
	char *node;
	int fd;
	int fd_id;
	bool drm;
	bool awake;
};

struct kmscon_seat {
	struct ev_eloop *eloop;
	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;

	struct uterm_monitor *mon;

	char *name;
	struct input *input;
	struct uterm_vt *vt;
	struct shl_dlist displays;
	struct shl_dlist videos;

	size_t session_count;
	struct shl_dlist sessions;

	bool awake;
	bool foreground;
	struct kmscon_session *current_sess;

	/* DPMS timeout management */
	struct ev_timer *dpms_timer;
	bool dpms_blanked;

	kmscon_seat_cb_t cb;
	void *data;
};

const char be_drm3d[] = "drm3d";
const char be_drm2d[] = "drm2d";
const char be_fbdev[] = "fbdev";
static int seat_video_init(struct kmscon_video *vid);
static int kmscon_seat_add_video(struct kmscon_seat *seat, enum uterm_monitor_dev_type type,
				 enum uterm_monitor_dev_flag flags, const char *node,
				 struct uterm_monitor_dev *udev);
static void kmscon_seat_remove_video(struct kmscon_seat *seat, void *data);
static void kmscon_seat_poll_video(void *data);

/* Forward declaration */
static void seat_dpms_reset_timer(struct kmscon_seat *seat);
static struct kmscon_session *kmscon_seat_new_session(struct kmscon_seat *seat);
static void kmscon_session_unregister(struct kmscon_session *sess);

static void activate_display(struct kmscon_display *d)
{
	int ret;
	struct shl_dlist *iter, *tmp;
	struct kmscon_session *s;
	struct kmscon_seat *seat = d->seat;

	if (d->activated || !d->seat->awake || !d->seat->foreground)
		return;

	/* TODO: We use the current KMS mode for new displays to avoid modesetting
	 * unless the user specifies not to, in which case we use the default mode,
	 * but we should also allow the user to specify different modes in the
	 * configuration files. */
	if (display_get_state(d->disp) == DISPLAY_ACTIVE) {
		d->activated = true;

		ret = display_set_dpms(d->disp, DPMS_ON);
		if (ret)
			log_warning("cannot set DPMS state to on for display: %d", ret);

		/* Reset DPMS timer when display becomes active */
		seat_dpms_reset_timer(seat);

		shl_dlist_for_each_safe(iter, tmp, &seat->sessions)
		{
			s = shl_dlist_entry(iter, struct kmscon_session, list);
			terminal_add_display(s->term, d->disp);
		}
	}
}

static int seat_go_foreground(struct kmscon_seat *seat)
{
	struct shl_dlist *iter, *tmp;
	struct kmscon_video *vid;
	struct kmscon_display *d;
	int ret;

	if (seat->foreground)
		return 0;
	if (!seat->awake)
		return -EBUSY;

	seat->foreground = true;

	shl_dlist_for_each_safe(iter, tmp, &seat->videos)
	{
		vid = shl_dlist_entry(iter, struct kmscon_video, list);
		if (!vid->video) {
			ret = seat_video_init(vid);
			if (ret) {
				shl_dlist_unlink(&vid->list);
				free(vid->node);
				free(vid);
			}
		}
	}

	shl_dlist_for_each(iter, &seat->videos)
	{
		vid = shl_dlist_entry(iter, struct kmscon_video, list);
		if (!vid->awake)
			video_wake_up(vid->video);
	}

	shl_dlist_for_each(iter, &seat->displays)
	{
		d = shl_dlist_entry(iter, struct kmscon_display, list);
		activate_display(d);
	}

	return 0;
}

static int seat_go_background(struct kmscon_seat *seat)
{
	struct shl_dlist *iter;
	struct kmscon_video *vid;

	if (!seat->foreground)
		return 0;
	if (!seat->awake)
		return -EBUSY;

	shl_dlist_for_each(iter, &seat->videos)
	{
		vid = shl_dlist_entry(iter, struct kmscon_video, list);
		video_sleep(vid->video);
	}

	seat->foreground = false;
	return 0;
}

static int seat_go_asleep(struct kmscon_seat *seat)
{
	if (!seat->awake)
		return 0;

	seat->awake = false;
	input_sleep(seat->input);

	return 0;
}

static int seat_go_awake(struct kmscon_seat *seat)
{
	int ret;

	if (seat->awake)
		return 0;

	if (seat->cb) {
		ret = seat->cb(seat, KMSCON_SEAT_WAKE_UP, seat->data);
		if (ret) {
			log_warning("cannot wake up seat %s: %d", seat->name, ret);
			return ret;
		}
	}

	seat->awake = true;
	input_wake_up(seat->input);

	return 0;
}

static void seat_switch(struct kmscon_seat *seat, struct kmscon_session *new)
{
	if (seat->current_sess == new)
		return;

	log_debug("switch session from %p to %p on seat %s", seat->current_sess, new, seat->name);

	if (seat->current_sess)
		terminal_deactivate(seat->current_sess->term);
	if (new) {
		if (new->foreground && !seat->foreground) {
			seat_go_foreground(seat);
		} else if (!new->foreground && seat->foreground) {
			seat_go_background(seat);
		}
		terminal_activate(new->term);
	}
	seat->current_sess = new;
}

static void seat_next(struct kmscon_seat *seat)
{
	struct kmscon_session *next = NULL;

	if (seat->current_sess)
		next = shl_dlist_next(seat->current_sess, &seat->sessions, list);
	if (!next)
		next = shl_dlist_first(&seat->sessions, struct kmscon_session, list);
	if (next && next == seat->current_sess)
		next = NULL;

	seat_switch(seat, next);
}

static void seat_prev(struct kmscon_seat *seat)
{
	struct kmscon_session *prev = NULL;

	if (seat->current_sess)
		prev = shl_dlist_prev(seat->current_sess, &seat->sessions, list);
	if (!prev)
		prev = shl_dlist_last(&seat->sessions, struct kmscon_session, list);
	if (prev && prev == seat->current_sess)
		prev = NULL;

	seat_switch(seat, prev);
}

static void seat_add_display(struct kmscon_seat *seat, struct display *disp)
{
	struct kmscon_display *d;

	log_debug("add display %s to seat %s", display_name(disp), seat->name);

	d = malloc(sizeof(*d));
	if (!d)
		return;
	memset(d, 0, sizeof(*d));
	d->disp = disp;
	d->seat = seat;

	display_ref(d->disp);
	shl_dlist_link(&seat->displays, &d->list);
	activate_display(d);
}

static void seat_remove_display(struct kmscon_seat *seat, struct kmscon_display *d)
{
	struct shl_dlist *iter, *tmp;
	struct kmscon_session *s;

	log_debug("remove display %s from seat %s", display_name(d->disp), seat->name);

	shl_dlist_unlink(&d->list);

	if (d->activated) {
		shl_dlist_for_each_safe(iter, tmp, &seat->sessions)
		{
			s = shl_dlist_entry(iter, struct kmscon_session, list);
			terminal_rm_display(s->term, d->disp);
		}
	}

	display_unref(d->disp);
	free(d);
}

static struct kmscon_display *seat_get_display(struct kmscon_seat *seat, struct display *disp)
{
	struct shl_dlist *iter;
	struct kmscon_display *d;

	shl_dlist_for_each(iter, &seat->displays)
	{
		d = shl_dlist_entry(iter, struct kmscon_display, list);
		if (d->disp == disp)
			return d;
	}
	return NULL;
}

static void seat_refresh_display(struct kmscon_seat *seat, struct kmscon_display *d)
{
	struct shl_dlist *iter;
	struct kmscon_session *s;

	log_debug("refresh display %s from seat %s", display_name(d->disp), seat->name);

	if (d->activated) {
		shl_dlist_for_each(iter, &seat->sessions)
		{
			s = shl_dlist_entry(iter, struct kmscon_session, list);
			terminal_refresh_displays(s->term);
		}
	}
}

static void seat_vt_activate(struct uterm_vt *vt, void *data)
{
	struct kmscon_seat *seat = data;

	log_debug("VT %d activated on seat %s", uterm_vt_get_num(vt), seat->name);

	if (seat_go_awake(seat))
		return;
	if (seat_go_foreground(seat))
		return;
	if (seat->current_sess)
		terminal_activate(seat->current_sess->term);
}

static int seat_vt_deactivate(struct uterm_vt *vt, bool force, void *data)
{
	struct kmscon_seat *seat = data;
	int ret;

	if (seat->current_sess)
		terminal_deactivate(seat->current_sess->term);

	ret = seat_go_background(seat);
	if (ret)
		return ret;
	ret = seat_go_asleep(seat);
	if (ret)
		return ret;
	return 0;
}

static void seat_vt_hup(struct uterm_vt *vt, void *data)
{
	struct kmscon_seat *seat = data;

	if (seat->cb)
		seat->cb(seat, KMSCON_SEAT_HUP, seat->data);
}

struct uterm_vt_cb seat_vt_cb = {
	.activate = seat_vt_activate,
	.deactivate = seat_vt_deactivate,
	.hup = seat_vt_hup,
};

static void seat_trigger_reboot(struct kmscon_seat *seat)
{
	FILE *fp;
	int ctrl_alt_del = 0; /* Default to soft reboot */

	/* Read system's ctrl-alt-del behavior setting */
	fp = fopen("/proc/sys/kernel/ctrl-alt-del", "r");
	if (fp) {
		if (fscanf(fp, "%d", &ctrl_alt_del) != 1) {
			log_warning(
				"failed to read ctrl-alt-del setting, defaulting to soft reboot");
			ctrl_alt_del = 0;
		}
		fclose(fp);
	} else {
		log_warning("failed to open /proc/sys/kernel/ctrl-alt-del: %m, defaulting to soft "
			    "reboot");
	}

	/* Soft reboot: signal init to handle graceful restart */
	if (ctrl_alt_del == 0) {
		log_warning("soft reboot triggered by keyboard shortcut on seat %s", seat->name);
		if (kill(1, SIGINT) < 0) {
			log_error("failed to signal PID 1 for reboot: %m");
		}
		return;
	}

	/* Hard reboot: immediate reboot */
	log_warning("hard reboot triggered by keyboard shortcut on seat %s", seat->name);
	sync(); /* Synchronize disk buffers */
	if (reboot(RB_AUTOBOOT) < 0) {
		log_error("failed to reboot system: %m");
	}
}
static void seat_dpms_timeout(struct ev_timer *timer, uint64_t num, void *data)
{
	struct kmscon_seat *seat = data;
	struct shl_dlist *iter;
	struct kmscon_display *d;
	int ret;

	if (!seat->conf->dpms_timeout || !seat->awake)
		return;

	log_debug("DPMS: blanking screen due to inactivity");

	/* Turn off all displays */
	shl_dlist_for_each(iter, &seat->displays)
	{
		d = shl_dlist_entry(iter, struct kmscon_display, list);

		/* Only set DPMS on activated displays */
		if (!d->activated)
			continue;
		ret = display_set_dpms(d->disp, DPMS_OFF);
		if (ret)
			log_warning("cannot set DPMS to OFF for display: %d", ret);
	}

	seat->dpms_blanked = true;
}

static void seat_dpms_reset_timer(struct kmscon_seat *seat)
{
	struct shl_dlist *iter;
	struct kmscon_display *d;
	struct itimerspec spec;
	int ret;

	if (!seat->conf->dpms_timeout || !seat->dpms_timer)
		return;

	/* If screen is blanked, unblank it */
	if (seat->dpms_blanked) {
		log_debug("DPMS: unblanking screen");
		shl_dlist_for_each(iter, &seat->displays)
		{
			d = shl_dlist_entry(iter, struct kmscon_display, list);

			/* Only set DPMS on activated displays */
			if (!d->activated)
				continue;
			ret = display_set_dpms(d->disp, DPMS_ON);
			if (ret)
				log_warning("cannot set DPMS to ON for display: %d", ret);
		}
		seat->dpms_blanked = false;
	}

	/* Reset the timer */
	memset(&spec, 0, sizeof(spec));
	spec.it_value.tv_sec = seat->conf->dpms_timeout;
	spec.it_value.tv_nsec = 0;
	ev_timer_update(seat->dpms_timer, &spec);
}

static void seat_pointer_event(struct input *input, struct input_pointer_event *ev, void *data)
{
	struct kmscon_seat *seat = data;

	/* Reset DPMS timer only on real user actions (not SYNC or HIDE_TIMEOUT) */
	if (ev->event != POINTER_MOVED && ev->event != POINTER_BUTTON && ev->event != POINTER_WHEEL)
		return;
	seat_dpms_reset_timer(seat);
}

static void seat_input_event(struct input *input, struct input_key_event *ev, void *data)
{
	struct kmscon_seat *seat = data;
	struct kmscon_session *s;

	/* Reset DPMS timer on any input event */
	seat_dpms_reset_timer(seat);

	if (ev->handled || !seat->awake)
		return;

	if (conf_grab_matches(seat->conf->grab_session_next, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control || seat->session_count < 2)
			return;
		seat_next(seat);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_session_prev, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control || seat->session_count < 2)
			return;
		seat_prev(seat);
		return;
	}

	if (conf_grab_matches(seat->conf->grab_session_close, ev->mods, ev->num_syms,
			      ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control || seat->session_count < 2)
			return;
		s = seat->current_sess;
		if (!s)
			return;

		kmscon_session_unregister(s);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_terminal_new, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		s = kmscon_seat_new_session(seat);
		if (s)
			seat_switch(seat, s);
		return;
	}
	if (seat->conf->grab_reboot &&
	    conf_grab_matches(seat->conf->grab_reboot, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		seat_trigger_reboot(seat);
		return;
	}
}

/*
 * Locale sucks, but we need its value for Compose support. Since we don't want
 * to set it (or reset it), and glibc doesn't provide any other sane way of
 * querying it, we just find it ourselves as described in locale(7).
 */
static const char *find_locale(void)
{
	const char *locale;

	locale = getenv("LC_ALL");
	if (!locale)
		locale = getenv("LC_CTYPE");
	if (!locale)
		locale = getenv("LANG");
	if (!locale)
		locale = "C";

	return locale;
}

static int kmscon_seat_set_keymap(struct kmscon_seat *seat)
{
	const char *locale;
	char *keymap, *compose_file;
	size_t compose_file_len;
	int ret;

	locale = find_locale();

	/* TODO: The XKB-API currently requires zero-terminated strings as
	 * keymap input. Hence, we have to read it in instead of using mmap().
	 * We should fix this upstream! */
	keymap = NULL;
	if (seat->conf->xkb_keymap && *seat->conf->xkb_keymap) {
		ret = shl_read_file(seat->conf->xkb_keymap, &keymap, NULL);
		if (ret)
			log_error("cannot read keymap file %s: %d", seat->conf->xkb_keymap, ret);
	}

	compose_file = NULL;
	compose_file_len = 0;
	if (seat->conf->xkb_compose_file && *seat->conf->xkb_compose_file) {
		ret = shl_read_file(seat->conf->xkb_compose_file, &compose_file, &compose_file_len);
		if (ret)
			log_error("cannot read compose file %s: %d", seat->conf->xkb_compose_file,
				  ret);
	}
	ret = input_set_keymap(seat->input, seat->conf->xkb_model, seat->conf->xkb_layout,
			       seat->conf->xkb_variant, seat->conf->xkb_options, locale, keymap,
			       compose_file, compose_file_len);
	if (ret)
		log_error("cannot set keymap: %d", ret);

	free(keymap);
	return ret;
}

static void kmscon_seat_add_input(struct kmscon_seat *seat, struct uterm_monitor_dev *udev,
				  const char *node)
{
	void *data;

	if (!seat || !node)
		return;

	data = input_add_dev(seat->input, node);
	uterm_monitor_set_dev_data(udev, data);
}

static void kmscon_seat_remove_input(struct kmscon_seat *seat, void *data)
{
	if (!seat || !data)
		return;

	input_remove_dev(seat->input, data);
}

static void seat_monitor_new_dev(const char *node, enum uterm_monitor_dev_type type,
				 enum uterm_monitor_dev_flag flags, void *data,
				 struct uterm_monitor_dev *udev)
{
	struct kmscon_seat *seat = data;

	switch (type) {
	case UTERM_MONITOR_DRM:
	case UTERM_MONITOR_FBDEV:
		kmscon_seat_add_video(seat, type, flags, node, udev);
		break;
	case UTERM_MONITOR_INPUT:
		log_debug("new input device %s", node);
		kmscon_seat_add_input(seat, udev, node);
		break;
	}
}

static void seat_monitor_free_dev(void *data, enum uterm_monitor_dev_type type, void *dev_data)
{
	struct kmscon_seat *seat = data;

	switch (type) {
	case UTERM_MONITOR_DRM:
	case UTERM_MONITOR_FBDEV:
		kmscon_seat_remove_video(seat, dev_data);
		break;
	case UTERM_MONITOR_INPUT:
		kmscon_seat_remove_input(seat, dev_data);
		break;
	}
}

static void seat_monitor_hotplug_dev(void *data, enum uterm_monitor_dev_type type, void *dev_data)
{
	if (type == UTERM_MONITOR_DRM || type == UTERM_MONITOR_FBDEV)
		kmscon_seat_poll_video(dev_data);
}

static struct uterm_monitor_cb seat_monitor_cb = {
	.new_dev = seat_monitor_new_dev,
	.free_dev = seat_monitor_free_dev,
	.hotplug_dev = seat_monitor_hotplug_dev,
};

int kmscon_seat_new(struct kmscon_seat **out, struct conf_ctx *main_conf,
		    struct kmscon_conf_t *conf, struct ev_eloop *eloop, kmscon_seat_cb_t cb,
		    void *data)
{
	struct kmscon_seat *seat;
	const char *seatname;
	int ret;

	if (!out || !eloop)
		return -EINVAL;

	seat = malloc(sizeof(*seat));
	if (!seat)
		return -ENOMEM;
	memset(seat, 0, sizeof(*seat));
	seat->eloop = eloop;
	seat->cb = cb;
	seat->data = data;
	shl_dlist_init(&seat->displays);
	shl_dlist_init(&seat->videos);
	shl_dlist_init(&seat->sessions);

	ret = input_new(&seat->input, seat->eloop);
	if (ret)
		goto err_free;

	seat->vt = uterm_vt_allocate(seat->eloop, conf->libseat, seat->input, conf->vt, &seat_vt_cb,
				     seat);
	if (!seat->vt) {
		ret = -ENOMEM;
		goto err_input;
	}
	seatname = uterm_vt_get_name(seat->vt);
	if (!seatname) {
		log_error("No name for the seat");
		ret = -EINVAL;
		goto err_vt;
	}
	seat->name = strdup(seatname);
	if (!seat->name) {
		ret = -ENOMEM;
		goto err_vt;
	}

	ret = kmscon_conf_new(&seat->conf_ctx);
	if (ret) {
		log_error("cannot create seat configuration object: %d", ret);
		goto err_name;
	}
	seat->conf = conf_ctx_get_mem(seat->conf_ctx);

	ret = kmscon_conf_load_seat(seat->conf_ctx, main_conf, seat->name);
	if (ret) {
		log_error("cannot parse seat configuration on seat %s: %d", seat->name, ret);
		goto err_conf;
	}

	input_set_conf(seat->input, seat->conf->xkb_repeat_delay, seat->conf->xkb_repeat_rate,
		       seat->conf->mouse);

	ret = kmscon_seat_set_keymap(seat);
	if (ret)
		goto err_conf;

	ret = uterm_monitor_new(&seat->mon, seat->eloop, &seat_monitor_cb, seat);
	if (ret) {
		log_error("cannot create device monitor: %d", ret);
		goto err_conf;
	}

	ret = input_register_key_cb(seat->input, seat_input_event, seat);
	if (ret)
		goto err_mon;

	/* Register pointer event handler for DPMS management */
	ret = input_register_pointer_cb(seat->input, seat_pointer_event, seat);
	if (ret) {
		log_warning("cannot register pointer callback: %d", ret);
		/* Not fatal, continue without pointer DPMS support */
	}

	/* Initialize DPMS timeout management */
	seat->dpms_timer = NULL;
	seat->dpms_blanked = false;

	/* Create DPMS timer if timeout is configured */
	if (seat->conf->dpms_timeout > 0) {
		struct itimerspec spec;
		ret = ev_eloop_new_timer(seat->eloop, &seat->dpms_timer, NULL, seat_dpms_timeout,
					 seat);
		if (ret) {
			log_warning("cannot create DPMS timer: %d", ret);
		} else {
			/* Start the timer */
			memset(&spec, 0, sizeof(spec));
			spec.it_value.tv_sec = seat->conf->dpms_timeout;
			ev_timer_update(seat->dpms_timer, &spec);
		}
	}

	if (seat->conf->session_control && uterm_vt_get_num(seat->vt) > 0) {
		log_warning("session control cannot be configured on real VT, disabling session "
			    "control");
		seat->conf->session_control = false;
	}

	ev_eloop_ref(seat->eloop);
	*out = seat;
	return 0;

err_mon:
	uterm_monitor_unref(seat->mon);
err_conf:
	kmscon_conf_free(seat->conf_ctx);
err_name:
	free(seat->name);
err_vt:
	uterm_vt_deallocate(seat->vt);
err_input:
	input_unref(seat->input);
err_free:
	free(seat);
	return ret;
}

void kmscon_seat_free(struct kmscon_seat *seat)
{
	struct kmscon_session *s;
	struct kmscon_video *vid;
	int ret;

	if (!seat)
		return;

	seat_switch(seat, NULL);

	ret = seat_go_asleep(seat);
	if (ret)
		log_warning("destroying seat %s while still awake: %d", seat->name, ret);

	while (!shl_dlist_empty(&seat->sessions)) {
		s = shl_dlist_entry(seat->sessions.next, struct kmscon_session, list);
		kmscon_session_unregister(s);
	}
	while (!shl_dlist_empty(&seat->videos)) {
		vid = shl_dlist_entry(seat->videos.next, struct kmscon_video, list);
		kmscon_seat_remove_video(seat, vid);
	}

	uterm_vt_deallocate(seat->vt);
	uterm_monitor_unref(seat->mon);
	input_unregister_key_cb(seat->input, seat_input_event, seat);
	input_unregister_pointer_cb(seat->input, seat_pointer_event, seat);
	input_unref(seat->input);
	kmscon_conf_free(seat->conf_ctx);
	free(seat->name);
	ev_eloop_rm_timer(seat->dpms_timer);
	ev_eloop_unref(seat->eloop);
	free(seat);
}

static void kmscon_seat_video_event(struct video *video, struct video_hotplug *ev, void *data)
{
	struct kmscon_video *vid = data;
	struct kmscon_display *d;

	log_debug("video event %d on video device %s on seat %s", ev->action, vid->node,
		  vid->seat->name);
	if (ev->action == VIDEO_NEW)
		return seat_add_display(vid->seat, ev->display);

	d = seat_get_display(vid->seat, ev->display);
	if (!d)
		return;

	switch (ev->action) {
	case VIDEO_GONE:
		seat_remove_display(vid->seat, d);
		break;
	case VIDEO_REFRESH:
		seat_refresh_display(vid->seat, d);
		break;
	}
}

static bool kmscon_seat_gpu_is_ignored(struct kmscon_seat *seat, unsigned int type, bool drm_backed,
				       bool primary, bool aux, const char *node)
{
	switch (type) {
	case UTERM_MONITOR_FBDEV:
		if (seat->conf->drm) {
			if (drm_backed) {
				log_info("ignoring video device %s on seat %s as it is a DRM-fbdev "
					 "device",
					 node, seat->name);
				return true;
			}
		}
		break;
	case UTERM_MONITOR_DRM:
		if (!seat->conf->drm) {
			log_info("ignoring video device %s on seat %s as it is a DRM device", node,
				 seat->name);
			return true;
		}
		break;
	default:
		log_info("ignoring unknown video device %s on seat %s", node, seat->name);
		return true;
	}

	if (seat->conf->gpus == KMSCON_GPU_PRIMARY && !primary) {
		log_info("ignoring video device %s on seat %s as it is no primary GPU", node,
			 seat->name);
		return true;
	}

	if (seat->conf->gpus == KMSCON_GPU_AUX && !primary && !aux) {
		log_info("ignoring video device %s on seat %s as it is neither a primary nor "
			 "auxiliary GPU",
			 node, seat->name);
		return true;
	}

	return false;
}

static int seat_video_init(struct kmscon_video *vid)
{
	struct kmscon_seat *seat = vid->seat;
	unsigned int width = 0;
	unsigned int height = 0;
	const char *backend;
	int ret;

	if (vid->drm) {
		if (seat->conf->hwaccel)
			backend = be_drm3d;
		else
			backend = be_drm2d;
	} else {
		backend = be_fbdev;
	}

	if (seat->conf->mode != NULL) {
		int items_parsed = sscanf(seat->conf->mode, "%ux%u", &width, &height);
		if (items_parsed != 2) {
			log_warning("The argument to --mode is not in the format <width>x<height>. "
				    "Ignoring");
			width = 0;
			height = 0;
		}
	}

	vid->fd = uterm_vt_open_device(seat->vt, vid->node, &vid->fd_id);
	if (vid->fd < 0) {
		log_error("cannot open video device %s on seat %s: %d", vid->node, seat->name,
			  vid->fd);
		return vid->fd;
	}

	ret = video_new(&vid->video, seat->eloop, vid->fd, backend, width, height,
			seat->conf->use_original_mode);
	if (ret && backend == be_drm3d) {
		log_info("cannot create drm3d device %s on seat %s (%d); trying drm2d mode",
			 vid->node, seat->name, ret);
		ret = video_new(&vid->video, seat->eloop, vid->fd, be_drm2d, width, height,
				seat->conf->use_original_mode);
	}
	if (ret) {
		log_error("cannot create video device %s on seat %s: %d", vid->node, seat->name,
			  ret);
		goto err_close;
	}

	ret = video_register_cb(vid->video, kmscon_seat_video_event, vid);
	if (ret) {
		log_error("cannot register video callback for device %s on seat %s: %d", vid->node,
			  seat->name, ret);
		goto err_video;
	}
	return video_wake_up(vid->video);

err_video:
	video_unref(vid->video);
err_close:
	uterm_vt_close_device(seat->vt, vid->fd, vid->fd_id);
	return ret;
}

static int kmscon_seat_add_video(struct kmscon_seat *seat, enum uterm_monitor_dev_type type,
				 enum uterm_monitor_dev_flag flags, const char *node,
				 struct uterm_monitor_dev *udev)
{
	struct kmscon_video *vid;
	int ret = -ENOMEM;

	if (kmscon_seat_gpu_is_ignored(seat, type, flags & UTERM_MONITOR_DRM_BACKED,
				       flags & UTERM_MONITOR_PRIMARY, flags & UTERM_MONITOR_AUX,
				       node))
		return -ERANGE;

	log_debug("new video device %s on seat %s", node, seat->name);

	vid = malloc(sizeof(*vid));
	if (!vid)
		return -ENOMEM;

	memset(vid, 0, sizeof(*vid));
	vid->seat = seat;
	vid->udev = udev;
	vid->drm = (type == UTERM_MONITOR_DRM);

	vid->node = strdup(node);
	if (!vid->node)
		goto err_free;

	uterm_monitor_set_dev_data(udev, vid);

	if (seat->awake) {
		ret = seat_video_init(vid);
		if (ret)
			goto err_node;
	}
	shl_dlist_link(&seat->videos, &vid->list);
	return 0;

err_node:
	free(vid->node);
err_free:
	free(vid);
	return ret;
}

static void kmscon_seat_remove_video(struct kmscon_seat *seat, void *data)
{
	struct kmscon_video *vid = data;
	struct display *disp;
	struct kmscon_display *d;

	if (!seat || !vid)
		return;

	log_debug("free video device %s on seat %s", vid->node, seat->name);

	uterm_monitor_set_dev_data(vid->udev, NULL);
	shl_dlist_unlink(&vid->list);
	video_unregister_cb(vid->video, kmscon_seat_video_event, vid);

	if (vid->video) {
		disp = video_get_displays(vid->video);
		while (disp) {
			d = seat_get_display(seat, disp);
			if (d)
				seat_remove_display(seat, d);
			disp = display_next(disp);
		}
		video_unref(vid->video);
		uterm_vt_close_device(seat->vt, vid->fd, vid->fd_id);
	}
	free(vid->node);
	free(vid);
}

static void kmscon_seat_poll_video(void *data)
{
	struct kmscon_video *vid = data;

	log_debug("poll video device %s on seat %s", vid->node, vid->seat->name);
	video_poll(vid->video);
}

void kmscon_seat_startup(struct kmscon_seat *seat)
{
	struct kmscon_session *s;

	if (!seat)
		return;

	seat_go_awake(seat);

	s = kmscon_seat_new_session(seat);
	if (s)
		seat_switch(seat, s);
	else
		log_error("cannot create new session on seat %s", seat->name);

	if (seat->conf->switchvt || uterm_vt_get_num(seat->vt) == 0)
		uterm_vt_activate(seat->vt);

	log_debug("scanning for devices...");
	uterm_monitor_scan(seat->mon, seat->name);
}

struct conf_ctx *kmscon_seat_get_conf(struct kmscon_seat *seat)
{
	if (!seat)
		return NULL;

	return seat->conf_ctx;
}

static struct kmscon_session *kmscon_seat_new_session(struct kmscon_seat *seat)
{
	struct kmscon_session *sess;
	struct shl_dlist *iter;
	struct kmscon_display *d;

	if (!seat)
		return NULL;

	if (seat->conf->session_max && seat->session_count >= seat->conf->session_max) {
		log_warning("maximum number of sessions reached (%d), dropping new session",
			    seat->conf->session_max);
		return NULL;
	}

	sess = malloc(sizeof(*sess));
	if (!sess) {
		log_error("cannot allocate memory for new session on seat %s", seat->name);
		return NULL;
	}

	log_debug("register session %p", sess);

	memset(sess, 0, sizeof(*sess));
	sess->seat = seat;
	sess->foreground = true;
	sess->term = terminal_new(sess, uterm_vt_get_num(seat->vt), seat->conf_ctx, seat->eloop,
				  seat->input, seat->name);
	if (!sess->term) {
		log_error("cannot create terminal for new session on seat %s", seat->name);
		free(sess);
		return NULL;
	}

	/* register new sessions next to the current one */
	if (seat->current_sess)
		shl_dlist_link(&seat->current_sess->list, &sess->list);
	else
		shl_dlist_link_tail(&seat->sessions, &sess->list);

	++seat->session_count;

	shl_dlist_for_each(iter, &seat->displays)
	{
		d = shl_dlist_entry(iter, struct kmscon_display, list);
		terminal_add_display(sess->term, d->disp);
	}
	return sess;
}

static void kmscon_session_unregister(struct kmscon_session *sess)
{
	struct kmscon_seat *seat;

	if (!sess || !sess->seat)
		return;

	log_debug("unregister session %p", sess);

	seat = sess->seat;

	if (seat->current_sess == sess) {
		terminal_deactivate(seat->current_sess->term);
		seat_next(seat);
	}

	shl_dlist_unlink(&sess->list);
	--seat->session_count;
	sess->seat = NULL;

	terminal_destroy(sess->term);
	free(sess);
}

int kmscon_session_set_foreground(struct kmscon_session *sess)
{
	struct kmscon_seat *seat;
	int ret;

	if (!sess)
		return -EINVAL;
	if (sess->foreground)
		return 0;

	seat = sess->seat;
	if (seat && seat->current_sess == sess && !seat->foreground) {
		ret = uterm_vt_restore(seat->vt);
		if (ret)
			return ret;

		ret = seat_go_foreground(seat);
		if (ret)
			return ret;
	}

	sess->foreground = true;
	return 0;
}

int kmscon_session_set_background(struct kmscon_session *sess)
{
	struct kmscon_seat *seat;
	int ret;

	if (!sess)
		return -EINVAL;
	if (!sess->foreground)
		return 0;

	seat = sess->seat;
	if (seat && seat->current_sess == sess && seat->foreground) {
		ret = seat_go_background(seat);
		if (ret)
			return ret;
	}

	sess->foreground = false;
	return 0;
}

bool kmscon_session_get_foreground(struct kmscon_session *sess)
{
	return sess->foreground;
}

void kmscon_session_bell(struct kmscon_session *sess)
{
	if (!sess || !sess->seat)
		return;

	uterm_vt_bell(sess->seat->vt);
}

void kmscon_session_set_leds(struct kmscon_session *sess, unsigned int scroll_lock,
			     unsigned int num_lock, unsigned int caps_lock)
{
	if (!sess || !sess->seat)
		return;

	input_set_leds(sess->seat->input, scroll_lock, num_lock, caps_lock);
}
