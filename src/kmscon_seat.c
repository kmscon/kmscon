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
#include "kmscon_dummy.h"
#include "kmscon_seat.h"
#include "kmscon_terminal.h"
#include "shl/dlist.h"
#include "shl/eloop.h"
#include "shl/log.h"
#include "uterm_vt.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "seat"

struct kmscon_session {
	struct shl_dlist list;
	unsigned long ref;
	struct kmscon_seat *seat;

	bool enabled;
	bool foreground;
	bool deactivating;

	struct ev_timer *timer;

	kmscon_session_cb_t cb;
	void *data;
};

struct kmscon_display {
	struct shl_dlist list;
	struct kmscon_seat *seat;
	struct uterm_display *disp;
	bool activated;
};

struct kmscon_video {
	struct shl_dlist list;
	struct kmscon_seat *seat;
	struct uterm_video *video;
	struct uterm_monitor_dev *udev;
	char *node;
	int fd;
	int fd_id;
	bool drm;
	bool awake;
};

enum kmscon_async_schedule {
	SCHEDULE_SWITCH,
	SCHEDULE_VT,
	SCHEDULE_UNREGISTER,
};

struct kmscon_seat {
	struct ev_eloop *eloop;
	struct uterm_vt_master *vtm;
	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;

	char *name;
	struct uterm_input *input;
	struct uterm_vt *vt;
	struct shl_dlist displays;
	struct shl_dlist videos;

	size_t session_count;
	struct shl_dlist sessions;

	bool awake;
	bool foreground;
	struct kmscon_session *current_sess;
	struct kmscon_session *scheduled_sess;
	struct kmscon_session *dummy_sess;

	unsigned int async_schedule;

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

static int session_call(struct kmscon_session *sess, unsigned int event, struct uterm_display *disp)
{
	struct kmscon_session_event ev;

	if (!sess->cb)
		return 0;

	memset(&ev, 0, sizeof(ev));
	ev.type = event;
	ev.disp = disp;
	return sess->cb(sess, &ev, sess->data);
}

static int session_call_activate(struct kmscon_session *sess)
{
	log_debug("activate session %p", sess);
	return session_call(sess, KMSCON_SESSION_ACTIVATE, NULL);
}

static int session_call_deactivate(struct kmscon_session *sess)
{
	log_debug("deactivate session %p", sess);
	return session_call(sess, KMSCON_SESSION_DEACTIVATE, NULL);
}

static void session_call_display_new(struct kmscon_session *sess, struct uterm_display *disp)
{
	session_call(sess, KMSCON_SESSION_DISPLAY_NEW, disp);
}

static void session_call_display_gone(struct kmscon_session *sess, struct uterm_display *disp)
{
	session_call(sess, KMSCON_SESSION_DISPLAY_GONE, disp);
}

static void session_call_display_refresh(struct kmscon_session *sess, struct uterm_display *disp)
{
	session_call(sess, KMSCON_SESSION_DISPLAY_REFRESH, disp);
}

/* Forward declaration */
static void seat_dpms_reset_timer(struct kmscon_seat *seat);

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
	if (uterm_display_get_state(d->disp) == UTERM_DISPLAY_ACTIVE) {
		d->activated = true;

		ret = uterm_display_set_dpms(d->disp, UTERM_DPMS_ON);
		if (ret)
			log_warning("cannot set DPMS state to on for display: %d", ret);

		/* Reset DPMS timer when display becomes active */
		seat_dpms_reset_timer(seat);

		shl_dlist_for_each_safe(iter, tmp, &seat->sessions)
		{
			s = shl_dlist_entry(iter, struct kmscon_session, list);
			session_call_display_new(s, d->disp);
		}
	}
}

static int seat_go_foreground(struct kmscon_seat *seat, bool force)
{
	struct shl_dlist *iter, *tmp;
	struct kmscon_video *vid;
	struct kmscon_display *d;
	int ret;

	if (seat->foreground)
		return 0;
	if (!seat->awake || (!force && seat->current_sess))
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
			uterm_video_wake_up(vid->video);
	}

	shl_dlist_for_each(iter, &seat->displays)
	{
		d = shl_dlist_entry(iter, struct kmscon_display, list);
		activate_display(d);
	}

	return 0;
}

static int seat_go_background(struct kmscon_seat *seat, bool force)
{
	struct shl_dlist *iter;
	struct kmscon_video *vid;

	if (!seat->foreground)
		return 0;
	if (!seat->awake || (!force && seat->current_sess))
		return -EBUSY;

	shl_dlist_for_each(iter, &seat->videos)
	{
		vid = shl_dlist_entry(iter, struct kmscon_video, list);
		uterm_video_sleep(vid->video);
	}

	seat->foreground = false;
	return 0;
}

static int seat_go_asleep(struct kmscon_seat *seat, bool force)
{
	int ret, err = 0;

	if (!seat->awake)
		return 0;
	if (seat->current_sess || seat->foreground) {
		if (force) {
			seat->foreground = false;
			seat->current_sess = NULL;
			err = -EBUSY;
		} else {
			return -EBUSY;
		}
	}

	if (seat->cb) {
		ret = seat->cb(seat, KMSCON_SEAT_SLEEP, seat->data);
		if (ret) {
			log_warning("cannot put seat %s asleep: %d", seat->name, ret);
			if (!force)
				return ret;
		}
	}

	seat->awake = false;
	uterm_input_sleep(seat->input);

	return err;
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
	uterm_input_wake_up(seat->input);

	return 0;
}

static int seat_run(struct kmscon_seat *seat)
{
	int ret;
	struct kmscon_session *session;

	if (!seat->awake)
		return -EBUSY;
	if (seat->current_sess)
		return 0;

	if (!seat->scheduled_sess) {
		log_debug("no session scheduled to run (num %zu)", seat->session_count);
		return -ENOENT;
	}
	session = seat->scheduled_sess;

	if (session->foreground && !seat->foreground) {
		ret = seat_go_foreground(seat, false);
		if (ret) {
			log_warning("cannot put seat %s into foreground for session %p", seat->name,
				    session);
			return ret;
		}
	} else if (!session->foreground && seat->foreground) {
		ret = seat_go_background(seat, false);
		if (ret) {
			log_warning("cannot put seat %s into background for session %p", seat->name,
				    session);
			return ret;
		}
	}

	ret = session_call_activate(session);
	if (ret) {
		log_warning("cannot activate session %p: %d", session, ret);
		return ret;
	}

	seat->current_sess = session;

	return 0;
}

static void session_deactivate(struct kmscon_session *sess)
{
	if (sess->seat->current_sess != sess)
		return;

	sess->seat->async_schedule = SCHEDULE_SWITCH;
	sess->deactivating = false;
	sess->seat->current_sess = NULL;
}

static int seat_pause(struct kmscon_seat *seat, bool force)
{
	int ret;

	if (!seat->current_sess)
		return 0;

	seat->current_sess->deactivating = true;
	ret = session_call_deactivate(seat->current_sess);
	if (ret) {
		if (ret == -EINPROGRESS)
			log_debug("pending deactivation for session %p", seat->current_sess);
		else
			log_warning("cannot deactivate session %p: %d", seat->current_sess, ret);
		if (!force)
			return ret;
	}

	session_deactivate(seat->current_sess);

	return ret;
}

static void seat_reschedule(struct kmscon_seat *seat)
{
	struct shl_dlist *iter, *start;
	struct kmscon_session *sess;

	if (seat->scheduled_sess && seat->scheduled_sess->enabled)
		return;

	if (seat->current_sess && seat->current_sess->enabled) {
		seat->scheduled_sess = seat->current_sess;
		return;
	}

	if (seat->current_sess)
		start = &seat->current_sess->list;
	else
		start = &seat->sessions;

	shl_dlist_for_each_but_one(iter, start, &seat->sessions)
	{
		sess = shl_dlist_entry(iter, struct kmscon_session, list);
		if (sess == seat->dummy_sess || !sess->enabled)
			continue;
		seat->scheduled_sess = sess;
		return;
	}

	if (seat->dummy_sess && seat->dummy_sess->enabled)
		seat->scheduled_sess = seat->dummy_sess;
	else
		seat->scheduled_sess = NULL;
}

static bool seat_has_schedule(struct kmscon_seat *seat)
{
	return seat->scheduled_sess && seat->scheduled_sess != seat->current_sess;
}

static int seat_switch(struct kmscon_seat *seat)
{
	int ret;

	seat->async_schedule = SCHEDULE_SWITCH;
	ret = seat_pause(seat, false);
	if (ret)
		return ret;

	return seat_run(seat);
}

static void seat_next(struct kmscon_seat *seat)
{
	struct shl_dlist *cur, *iter;
	struct kmscon_session *s, *next;

	if (seat->current_sess)
		cur = &seat->current_sess->list;
	else if (seat->session_count)
		cur = &seat->sessions;
	else
		return;

	next = NULL;
	if (!seat->current_sess && seat->dummy_sess && seat->dummy_sess->enabled)
		next = seat->dummy_sess;

	shl_dlist_for_each_but_one(iter, cur, &seat->sessions)
	{
		s = shl_dlist_entry(iter, struct kmscon_session, list);
		if (!s->enabled || seat->dummy_sess == s)
			continue;

		next = s;
		break;
	}

	if (!next)
		return;

	seat->scheduled_sess = next;
	seat_switch(seat);
}

static void seat_prev(struct kmscon_seat *seat)
{
	struct shl_dlist *cur, *iter;
	struct kmscon_session *s, *prev;

	if (seat->current_sess)
		cur = &seat->current_sess->list;
	else if (seat->session_count)
		cur = &seat->sessions;
	else
		return;

	prev = NULL;
	if (!seat->current_sess && seat->dummy_sess && seat->dummy_sess->enabled)
		prev = seat->dummy_sess;

	shl_dlist_for_each_reverse_but_one(iter, cur, &seat->sessions)
	{
		s = shl_dlist_entry(iter, struct kmscon_session, list);
		if (!s->enabled || seat->dummy_sess == s)
			continue;

		prev = s;
		break;
	}

	if (!prev)
		return;

	seat->scheduled_sess = prev;
	seat_switch(seat);
}

static void seat_add_display(struct kmscon_seat *seat, struct uterm_display *disp)
{
	struct kmscon_display *d;

	log_debug("add display %s to seat %s", uterm_display_name(disp), seat->name);

	d = malloc(sizeof(*d));
	if (!d)
		return;
	memset(d, 0, sizeof(*d));
	d->disp = disp;
	d->seat = seat;

	uterm_display_ref(d->disp);
	shl_dlist_link(&seat->displays, &d->list);
	activate_display(d);
}

static void seat_remove_display(struct kmscon_seat *seat, struct kmscon_display *d)
{
	struct shl_dlist *iter, *tmp;
	struct kmscon_session *s;

	log_debug("remove display %s from seat %s", uterm_display_name(d->disp), seat->name);

	shl_dlist_unlink(&d->list);

	if (d->activated) {
		shl_dlist_for_each_safe(iter, tmp, &seat->sessions)
		{
			s = shl_dlist_entry(iter, struct kmscon_session, list);
			session_call_display_gone(s, d->disp);
		}
	}

	uterm_display_unref(d->disp);
	free(d);
}

static struct kmscon_display *seat_get_display(struct kmscon_seat *seat, struct uterm_display *disp)
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

	log_debug("refresh display %s from seat %s", uterm_display_name(d->disp), seat->name);

	if (d->activated) {
		shl_dlist_for_each(iter, &seat->sessions)
		{
			s = shl_dlist_entry(iter, struct kmscon_session, list);
			session_call_display_refresh(s, d->disp);
		}
	}
}

static int seat_vt_event(struct uterm_vt *vt, struct uterm_vt_event *ev, void *data)
{
	struct kmscon_seat *seat = data;
	int ret;

	switch (ev->action) {
	case UTERM_VT_ACTIVATE:
		ret = seat_go_awake(seat);
		if (ret)
			return ret;
		seat_run(seat);
		break;
	case UTERM_VT_DEACTIVATE:
		seat->async_schedule = SCHEDULE_VT;
		ret = seat_pause(seat, false);
		if (ret)
			return ret;
		ret = seat_go_background(seat, false);
		if (ret)
			return ret;
		ret = seat_go_asleep(seat, false);
		if (ret)
			return ret;
		break;
	case UTERM_VT_HUP:
		if (seat->cb)
			seat->cb(seat, KMSCON_SEAT_HUP, seat->data);
		break;
	}

	return 0;
}

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

	if (!seat->conf->dpms_timeout)
		return;

	log_debug("DPMS: blanking screen due to inactivity");

	/* Turn off all displays */
	shl_dlist_for_each(iter, &seat->displays)
	{
		d = shl_dlist_entry(iter, struct kmscon_display, list);

		/* Only set DPMS on activated displays */
		if (!d->activated)
			continue;
		ret = uterm_display_set_dpms(d->disp, UTERM_DPMS_OFF);
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
			ret = uterm_display_set_dpms(d->disp, UTERM_DPMS_ON);
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

static void seat_pointer_event(struct uterm_input *input, struct uterm_input_pointer_event *ev,
			       void *data)
{
	struct kmscon_seat *seat = data;

	/* Reset DPMS timer only on real user actions (not SYNC or HIDE_TIMEOUT) */
	if (ev->event != UTERM_MOVED && ev->event != UTERM_BUTTON && ev->event != UTERM_WHEEL)
		return;
	seat_dpms_reset_timer(seat);
}

static void seat_input_event(struct uterm_input *input, struct uterm_input_key_event *ev,
			     void *data)
{
	struct kmscon_seat *seat = data;
	struct kmscon_session *s;
	int ret;

	/* Reset DPMS timer on any input event */
	seat_dpms_reset_timer(seat);

	if (ev->handled || !seat->awake)
		return;

	if (conf_grab_matches(seat->conf->grab_session_next, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		seat_next(seat);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_session_prev, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		seat_prev(seat);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_session_dummy, ev->mods, ev->num_syms,
			      ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		seat->scheduled_sess = seat->dummy_sess;
		seat_switch(seat);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_session_close, ev->mods, ev->num_syms,
			      ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		s = seat->current_sess;
		if (!s)
			return;
		if (s == seat->dummy_sess)
			return;

		/* First time this is invoked on a session, we simply try
		 * unloading it. If it fails, we give it some time. If this is
		 * invoked a second time, we notice that we already tried
		 * removing it and so we go straight to unregistering the
		 * session unconditionally. */
		if (!s->deactivating) {
			seat->async_schedule = SCHEDULE_UNREGISTER;
			ret = seat_pause(seat, false);
			if (ret)
				return;
		}

		kmscon_session_unregister(s);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_terminal_new, ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		ret = kmscon_terminal_register(&s, seat, uterm_vt_get_num(seat->vt));
		if (ret == -EOPNOTSUPP) {
			log_notice("terminal support not compiled in");
		} else if (ret) {
			log_error("cannot register terminal session: %d", ret);
		} else {
			s->enabled = true;
			seat->scheduled_sess = s;
			seat_switch(seat);
		}
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

int kmscon_seat_new(struct kmscon_seat **out, struct conf_ctx *main_conf, struct ev_eloop *eloop,
		    struct uterm_vt_master *vtm, bool listen, const char *seatname,
		    kmscon_seat_cb_t cb, void *data)
{
	struct kmscon_seat *seat;
	int ret;
	const char *locale;
	char *keymap, *compose_file;
	size_t compose_file_len;

	if (!out || !eloop || !vtm || !seatname)
		return -EINVAL;

	seat = malloc(sizeof(*seat));
	if (!seat)
		return -ENOMEM;
	memset(seat, 0, sizeof(*seat));
	seat->eloop = eloop;
	seat->vtm = vtm;
	seat->cb = cb;
	seat->data = data;
	shl_dlist_init(&seat->displays);
	shl_dlist_init(&seat->videos);
	shl_dlist_init(&seat->sessions);

	seat->name = strdup(seatname);
	if (!seat->name) {
		log_error("cannot copy string");
		ret = -ENOMEM;
		goto err_free;
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

	ret = uterm_input_new(&seat->input, seat->eloop, seat->conf->xkb_model,
			      seat->conf->xkb_layout, seat->conf->xkb_variant,
			      seat->conf->xkb_options, locale, keymap, compose_file,
			      compose_file_len, seat->conf->xkb_repeat_delay,
			      seat->conf->xkb_repeat_rate, seat->conf->mouse);
	free(keymap);

	if (ret)
		goto err_conf;

	ret = uterm_input_register_key_cb(seat->input, seat_input_event, seat);
	if (ret)
		goto err_input;

	/* Register pointer event handler for DPMS management */
	ret = uterm_input_register_pointer_cb(seat->input, seat_pointer_event, seat);
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

	ret = uterm_vt_allocate(seat->vtm, &seat->vt, listen, seat->name, seat->input,
				seat->conf->vt, seat_vt_event, seat);
	if (ret)
		goto err_input_cb;

	if (seat->conf->session_control && uterm_vt_get_num(seat->vt) > 0) {
		log_warning("session control cannot be configured on real VT, disabling session "
			    "control");
		seat->conf->session_control = false;
	}

	ev_eloop_ref(seat->eloop);
	uterm_vt_master_ref(seat->vtm);
	*out = seat;
	return 0;

err_input_cb:
	ev_eloop_rm_timer(seat->dpms_timer);
	uterm_input_unregister_key_cb(seat->input, seat_input_event, seat);
	uterm_input_unregister_pointer_cb(seat->input, seat_pointer_event, seat);
err_input:
	uterm_input_unref(seat->input);
err_conf:
	kmscon_conf_free(seat->conf_ctx);
err_name:
	free(seat->name);
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

	ret = seat_pause(seat, true);
	if (ret)
		log_warning("destroying seat %s while session %p is active", seat->name,
			    seat->current_sess);

	ret = seat_go_asleep(seat, true);
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
	uterm_input_unregister_key_cb(seat->input, seat_input_event, seat);
	uterm_input_unregister_pointer_cb(seat->input, seat_pointer_event, seat);
	uterm_input_unref(seat->input);
	kmscon_conf_free(seat->conf_ctx);
	free(seat->name);
	uterm_vt_master_unref(seat->vtm);
	ev_eloop_rm_timer(seat->dpms_timer);
	ev_eloop_unref(seat->eloop);
	free(seat);
}

static void kmscon_seat_video_event(struct uterm_video *video, struct uterm_video_hotplug *ev,
				    void *data)
{
	struct kmscon_video *vid = data;
	struct kmscon_display *d;

	log_debug("video event %d on video device %s on seat %s", ev->action, vid->node,
		  vid->seat->name);
	if (ev->action == UTERM_NEW)
		return seat_add_display(vid->seat, ev->display);

	d = seat_get_display(vid->seat, ev->display);
	if (!d)
		return;

	switch (ev->action) {
	case UTERM_GONE:
		seat_remove_display(vid->seat, d);
		break;
	case UTERM_REFRESH:
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

	ret = uterm_video_new(&vid->video, seat->eloop, vid->fd, backend, width, height,
			      seat->conf->use_original_mode);
	if (ret && backend == be_drm3d) {
		log_info("cannot create drm3d device %s on seat %s (%d); trying drm2d mode",
			 vid->node, seat->name, ret);
		ret = uterm_video_new(&vid->video, seat->eloop, vid->fd, be_drm2d, width, height,
				      seat->conf->use_original_mode);
	}
	if (ret) {
		log_error("cannot create video device %s on seat %s: %d", vid->node, seat->name,
			  ret);
		goto err_close;
	}

	ret = uterm_video_register_cb(vid->video, kmscon_seat_video_event, vid);
	if (ret) {
		log_error("cannot register video callback for device %s on seat %s: %d", vid->node,
			  seat->name, ret);
		goto err_video;
	}
	return uterm_video_wake_up(vid->video);

err_video:
	uterm_video_unref(vid->video);
err_close:
	uterm_vt_close_device(seat->vt, vid->fd, vid->fd_id);
	return ret;
}

int kmscon_seat_add_video(struct kmscon_seat *seat, unsigned int type, unsigned int flags,
			  const char *node, struct uterm_monitor_dev *udev)
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

void kmscon_seat_remove_video(struct kmscon_seat *seat, void *data)
{
	struct kmscon_video *vid = data;
	struct uterm_display *disp;
	struct kmscon_display *d;

	log_debug("free video device %s on seat %s", vid->node, seat->name);

	shl_dlist_unlink(&vid->list);
	uterm_video_unregister_cb(vid->video, kmscon_seat_video_event, vid);

	if (vid->video) {
		disp = uterm_video_get_displays(vid->video);
		while (disp) {
			d = seat_get_display(seat, disp);
			if (d)
				seat_remove_display(seat, d);
			disp = uterm_display_next(disp);
		}
		uterm_video_unref(vid->video);
		uterm_vt_close_device(seat->vt, vid->fd, vid->fd_id);
	}
	free(vid->node);
	free(vid);
}

void kmscon_seat_poll_video(void *data)
{
	struct kmscon_video *vid = data;

	log_debug("poll video device %s on seat %s", vid->node, vid->seat->name);
	uterm_video_poll(vid->video);
}

void kmscon_seat_startup(struct kmscon_seat *seat)
{
	int ret;
	struct kmscon_session *s;

	if (!seat)
		return;

	ret = kmscon_dummy_register(&s, seat);
	if (ret == -EOPNOTSUPP) {
		log_notice("dummy sessions not compiled in");
	} else if (ret) {
		log_error("cannot register dummy session: %d", ret);
	} else {
		seat->dummy_sess = s;
		kmscon_session_enable(s);
	}

	if (seat->conf->terminal_session) {
		ret = kmscon_terminal_register(&s, seat, uterm_vt_get_num(seat->vt));
		if (ret == -EOPNOTSUPP)
			log_notice("terminal support not compiled in");
		else if (ret)
			log_error("cannot register terminal session");
		else
			kmscon_session_enable(s);
	}

	if (seat->conf->switchvt || uterm_vt_get_num(seat->vt) == 0)
		uterm_vt_activate(seat->vt);
}

int kmscon_seat_add_input(struct kmscon_seat *seat, const char *node)
{
	if (!seat || !node)
		return -EINVAL;

	uterm_input_add_dev(seat->input, node);
	return 0;
}

void kmscon_seat_remove_input(struct kmscon_seat *seat, const char *node)
{
	if (!seat || !node)
		return;

	uterm_input_remove_dev(seat->input, node);
}

const char *kmscon_seat_get_name(struct kmscon_seat *seat)
{
	if (!seat)
		return NULL;

	return seat->name;
}

struct uterm_input *kmscon_seat_get_input(struct kmscon_seat *seat)
{
	if (!seat)
		return NULL;

	return seat->input;
}

struct ev_eloop *kmscon_seat_get_eloop(struct kmscon_seat *seat)
{
	if (!seat)
		return NULL;

	return seat->eloop;
}

struct conf_ctx *kmscon_seat_get_conf(struct kmscon_seat *seat)
{
	if (!seat)
		return NULL;

	return seat->conf_ctx;
}

int kmscon_seat_register_session(struct kmscon_seat *seat, struct kmscon_session **out,
				 kmscon_session_cb_t cb, void *data)
{
	struct kmscon_session *sess;
	struct shl_dlist *iter;
	struct kmscon_display *d;

	if (!seat || !out)
		return -EINVAL;

	if (seat->conf->session_max && seat->session_count >= seat->conf->session_max) {
		log_warning("maximum number of sessions reached (%d), dropping new session",
			    seat->conf->session_max);
		return -EOVERFLOW;
	}

	sess = malloc(sizeof(*sess));
	if (!sess) {
		log_error("cannot allocate memory for new session on seat %s", seat->name);
		return -ENOMEM;
	}

	log_debug("register session %p", sess);

	memset(sess, 0, sizeof(*sess));
	sess->ref = 1;
	sess->seat = seat;
	sess->cb = cb;
	sess->data = data;
	sess->foreground = true;

	/* register new sessions next to the current one */
	if (seat->current_sess)
		shl_dlist_link(&seat->current_sess->list, &sess->list);
	else
		shl_dlist_link_tail(&seat->sessions, &sess->list);

	++seat->session_count;
	*out = sess;

	shl_dlist_for_each(iter, &seat->displays)
	{
		d = shl_dlist_entry(iter, struct kmscon_display, list);
		session_call_display_new(sess, d->disp);
	}

	return 0;
}

void kmscon_session_ref(struct kmscon_session *sess)
{
	if (!sess || !sess->ref)
		return;

	++sess->ref;
}

void kmscon_session_unref(struct kmscon_session *sess)
{
	if (!sess || !sess->ref || --sess->ref)
		return;

	kmscon_session_unregister(sess);
	free(sess);
}

void kmscon_session_unregister(struct kmscon_session *sess)
{
	struct kmscon_seat *seat;
	int ret;
	bool forced = false;

	if (!sess || !sess->seat)
		return;

	log_debug("unregister session %p", sess);

	seat = sess->seat;
	sess->enabled = false;
	if (seat->dummy_sess == sess)
		seat->dummy_sess = NULL;
	seat_reschedule(seat);

	if (seat->current_sess == sess) {
		ret = seat_pause(seat, true);
		if (ret) {
			forced = true;
			log_warning("unregistering active session %p; skipping automatic "
				    "session-switch",
				    sess);
		}
	}

	shl_dlist_unlink(&sess->list);
	--seat->session_count;
	sess->seat = NULL;

	session_call(sess, KMSCON_SESSION_UNREGISTER, NULL);
	kmscon_session_unref(sess);

	/* If this session was active and we couldn't deactivate it, then it
	 * might still have resources allocated that couldn't get freed. In this
	 * case we should not automatically switch to the next session as it is
	 * very likely that it will not be able to start.
	 * Instead, we stay inactive and wait for user/external input to switch
	 * to another session. This delay will then hopefully be long enough so
	 * all resources got freed. */
	if (!forced)
		seat_run(seat);
}

bool kmscon_session_is_registered(struct kmscon_session *sess)
{
	return sess && sess->seat;
}

bool kmscon_session_is_active(struct kmscon_session *sess)
{
	return sess && sess->seat && sess->seat->current_sess == sess;
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

		ret = seat_go_foreground(seat, true);
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
		ret = seat_go_background(seat, true);
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

void kmscon_session_enable(struct kmscon_session *sess)
{
	if (!sess || sess->enabled)
		return;

	log_debug("enable session %p", sess);
	sess->enabled = true;
	if (sess->seat &&
	    (!sess->seat->current_sess || sess->seat->current_sess == sess->seat->dummy_sess)) {
		sess->seat->scheduled_sess = sess;
		if (seat_has_schedule(sess->seat))
			seat_switch(sess->seat);
	}
}

void kmscon_session_disable(struct kmscon_session *sess)
{
	if (!sess || !sess->enabled)
		return;

	log_debug("disable session %p", sess);
	sess->enabled = false;
}

bool kmscon_session_is_enabled(struct kmscon_session *sess)
{
	return sess && sess->enabled;
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

	uterm_input_set_leds(sess->seat->input, scroll_lock, num_lock, caps_lock);
}

void kmscon_session_notify_deactivated(struct kmscon_session *sess)
{
	struct kmscon_seat *seat;
	int ret;
	unsigned int sched;

	if (!sess || !sess->seat)
		return;

	seat = sess->seat;
	if (seat->current_sess != sess)
		return;

	sched = seat->async_schedule;
	log_debug("session %p notified core about deactivation (schedule: %u)", sess, sched);
	session_deactivate(sess);
	seat_reschedule(seat);

	if (sched == SCHEDULE_VT) {
		ret = seat_go_background(seat, false);
		if (ret)
			return;
		ret = seat_go_asleep(seat, false);
		if (ret)
			return;
		uterm_vt_retry(seat->vt);
	} else if (sched == SCHEDULE_UNREGISTER) {
		kmscon_session_unregister(sess);
	} else {
		seat_switch(seat);
	}
}
