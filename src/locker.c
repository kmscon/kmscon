/*
 * kmscon - Terminal locker
 *
 * Copyright (c) 2026 Red Hat.
 * Author: Jocelyn Falempe <jfalempe@redhat.com>
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

#include <errno.h>
#include <libtsm.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "config.h"
#include "input/input.h"
#include "locker.h"
#include "shl/eloop.h"
#include "shl/log.h"
#include "shl/misc.h"

#ifdef BUILD_ENABLE_PAM
#include <security/pam_appl.h>
#endif

#define LOG_SUBSYSTEM "locker"
#define LOCKER_PAM_SERVICE "kmscon-lock"
#define MAX_PASS 512

struct locker {
	/* typed password */
	char buf[MAX_PASS];
	size_t len;

	struct ev_eloop *eloop;
	char *user;
	char *tty;
	/* authentication child, 0 if none is running */
	pid_t pid;

	/* lock screen */
	struct tsm_screen *console;
	struct tsm_vte *vte;
	const char *status;

	locker_cb cb;
	void *data;
};

/*
 * Lock screen
 */

static void locker_write_event(struct tsm_vte *vte, const char *u8, size_t len, void *data) {}

/* Only updates the screen, the caller is responsible for the redraw */
static void locker_draw(struct locker *locker, const char *status)
{
	char buf[512];
	int len;

	locker->status = status;
	len = snprintf(buf, sizeof(buf),
		       "\033[H\033[2J"
		       "This terminal is locked.\r\n\r\n"
		       "%s%s"
		       "Password for %s: ",
		       status ? status : "", status ? "\r\n\r\n" : "", locker->user);
	if (len < 0)
		return;
	if ((size_t)len >= sizeof(buf))
		len = sizeof(buf) - 1;

	tsm_vte_input(locker->vte, buf, len);
}

static void locker_update(struct locker *locker, const char *status)
{
	locker_draw(locker, status);
	locker->cb(locker, LOCKER_REDRAW, locker->data);
}

/*
 * Authentication
 * The PAM conversation runs in a child process, so a slow authentication
 * (failure delay, network backends...) doesn't block kmscon.
 */

static void clear_password(struct locker *locker)
{
	explicit_bzero(locker->buf, sizeof(locker->buf));
	locker->len = 0;
}

#ifdef BUILD_ENABLE_PAM

static void free_responses(struct pam_response *resp, int num)
{
	int i;

	for (i = 0; i < num; i++) {
		if (resp[i].resp) {
			explicit_bzero(resp[i].resp, strlen(resp[i].resp));
			free(resp[i].resp);
		}
	}
	free(resp);
}

/* Answer the password prompts with the typed password. Other questions cannot
 * be answered, as the password is collected before the conversation starts. */
static int pam_conv_cb(int num, const struct pam_message **msg, struct pam_response **resp,
		       void *data)
{
	struct locker *locker = data;
	struct pam_response *r;
	int i;

	if (num <= 0 || num > PAM_MAX_NUM_MSG)
		return PAM_CONV_ERR;

	r = calloc(num, sizeof(*r));
	if (!r)
		return PAM_BUF_ERR;

	for (i = 0; i < num; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			r[i].resp = strndup(locker->buf, locker->len);
			if (!r[i].resp) {
				free_responses(r, num);
				return PAM_BUF_ERR;
			}
			break;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			break;
		default:
			free_responses(r, num);
			return PAM_CONV_ERR;
		}
	}

	*resp = r;
	return PAM_SUCCESS;
}

static void __attribute__((noreturn)) child_authenticate(struct locker *locker)
{
	struct pam_conv conv = {pam_conv_cb, locker};
	pam_handle_t *pamh;
	sigset_t mask;
	int ret;

	/* Don't outlive kmscon */
	prctl(PR_SET_PDEATHSIG, SIGKILL);

	/* The eloop blocks the signals it handles with signalfd */
	signal(SIGPIPE, SIG_IGN);
	sigemptyset(&mask);
	sigprocmask(SIG_SETMASK, &mask, NULL);

	ret = pam_start(LOCKER_PAM_SERVICE, locker->user, &conv, &pamh);
	if (ret != PAM_SUCCESS)
		_exit(2);

	if (locker->tty)
		pam_set_item(pamh, PAM_TTY, locker->tty);

	ret = pam_authenticate(pamh, PAM_SILENT);
	clear_password(locker);
	pam_end(pamh, ret);

	_exit(ret == PAM_SUCCESS ? 0 : 1);
}

static void child_done(struct ev_eloop *eloop, struct ev_child_data *chld, void *data)
{
	struct locker *locker = data;
	bool success;

	if (!locker->pid || chld->pid != locker->pid)
		return;

	success = WIFEXITED(chld->status) && WEXITSTATUS(chld->status) == 0;
	if (WIFEXITED(chld->status) && WEXITSTATUS(chld->status) == 2)
		log_error("cannot start PAM service " LOCKER_PAM_SERVICE);

	locker->pid = 0;
	ev_eloop_unregister_child_cb(locker->eloop, child_done, locker);

	if (success) {
		log_notice("terminal unlocked by %s", locker->user);
		/* last, as it may free the locker */
		locker->cb(locker, LOCKER_UNLOCKED, locker->data);
	} else {
		log_notice("authentication failure for %s", locker->user);
		locker_update(locker, "Authentication failed.");
	}
}

/* Returns 1 when the authentication starts, or a negative error code */
static int locker_authenticate(struct locker *locker)
{
	pid_t pid;
	int ret;

	ret = ev_eloop_register_child_cb(locker->eloop, child_done, locker);
	if (ret) {
		clear_password(locker);
		return ret;
	}

	/* SIGCHLD is handled by the eloop, so child_done() cannot run before
	 * locker->pid is set */
	pid = fork();
	if (pid < 0) {
		ret = -errno;
		log_error("cannot fork authentication process: %m");
		ev_eloop_unregister_child_cb(locker->eloop, child_done, locker);
		clear_password(locker);
		return ret;
	}
	if (pid == 0)
		child_authenticate(locker);

	locker->pid = pid;
	clear_password(locker);
	return 1;
}

static void locker_kill_child(struct locker *locker)
{
	if (!locker->pid)
		return;

	/* The eloop still reaps it */
	kill(locker->pid, SIGKILL);
	ev_eloop_unregister_child_cb(locker->eloop, child_done, locker);
	locker->pid = 0;
}

#else /* BUILD_ENABLE_PAM */

static int locker_authenticate(struct locker *locker)
{
	clear_password(locker);
	return -EOPNOTSUPP;
}

static void locker_kill_child(struct locker *locker) {}

#endif /* BUILD_ENABLE_PAM */

/*
 * Password input
 */

static void remove_last_char(struct locker *locker)
{
	/* skip UTF-8 continuation bytes */
	while (locker->len > 0 && (locker->buf[locker->len - 1] & 0xc0) == 0x80)
		locker->len--;
	if (locker->len > 0)
		locker->len--;
	explicit_bzero(&locker->buf[locker->len], sizeof(locker->buf) - locker->len);
}

static void add_char(struct locker *locker, uint32_t codepoint, unsigned int mods)
{
	char u8[4];
	size_t len;

	/* also rejects INPUT_INVALID */
	if (codepoint < 0x20 || codepoint == 0x7f || codepoint > 0x10ffff)
		return;
	if (mods & (SHL_CONTROL_MASK | SHL_ALT_MASK | SHL_LOGO_MASK))
		return;

	len = tsm_ucs4_to_utf8(codepoint, u8);
	/* keep a trailing NUL byte */
	if (len && locker->len + len < sizeof(locker->buf)) {
		memcpy(&locker->buf[locker->len], u8, len);
		locker->len += len;
	}
	explicit_bzero(u8, sizeof(u8));
}

void locker_input(struct locker *locker, struct input_key_event *ev)
{
	uint32_t keysym;
	int ret;

	/* Ignore typing while the password is checked */
	if (ev->num_syms != 1 || locker->pid)
		return;

	keysym = ev->keysyms[0];
	switch (keysym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		ret = locker_authenticate(locker);
		if (ret > 0)
			locker_update(locker, "Authenticating...");
		else
			locker_update(locker, "Cannot start the authentication.");
		return;
	case XKB_KEY_BackSpace:
		remove_last_char(locker);
		return;
	case XKB_KEY_Escape:
		clear_password(locker);
		return;
	}

	if ((ev->mods & SHL_CONTROL_MASK) && (keysym == XKB_KEY_u || keysym == XKB_KEY_U)) {
		clear_password(locker);
		return;
	}

	add_char(locker, ev->codepoints[0], ev->mods);
}

/*
 * Locker
 */

struct locker *locker_new(struct ev_eloop *eloop, struct kmscon_conf_t *conf, const char *user,
			  const char *tty, unsigned int cols, unsigned int rows, locker_cb cb,
			  void *data)
{
	struct locker *locker;

	if (!eloop || !user || !cb)
		return NULL;

#ifndef BUILD_ENABLE_PAM
	log_warning("kmscon was built without PAM support, terminals cannot be locked");
	return NULL;
#endif

	locker = calloc(1, sizeof(*locker));
	if (!locker)
		return NULL;

	/* Best effort, to keep the password out of swap */
	mlock(locker->buf, sizeof(locker->buf));

	locker->eloop = eloop;
	ev_eloop_ref(locker->eloop);
	locker->cb = cb;
	locker->data = data;

	locker->user = strdup(user);
	if (!locker->user)
		goto err;
	if (tty) {
		locker->tty = strdup(tty);
		if (!locker->tty)
			goto err;
	}

	if (tsm_screen_new(&locker->console, log_llog, NULL))
		goto err;

	tsm_screen_set_max_sb(locker->console, 0);
	if (cols && rows)
		tsm_screen_resize(locker->console, cols, rows);

	if (tsm_vte_new(&locker->vte, locker->console, locker_write_event, locker, log_llog, NULL))
		goto err;

	if (tsm_vte_set_palette(locker->vte, conf->palette) ||
	    tsm_vte_set_custom_palette(locker->vte, conf->custom_palette))
		goto err;

	locker_draw(locker, NULL);
	return locker;

err:
	locker_free(locker);
	return NULL;
}

void locker_free(struct locker *locker)
{
	if (!locker)
		return;

	locker_kill_child(locker);
	clear_password(locker);
	munlock(locker->buf, sizeof(locker->buf));
	tsm_vte_unref(locker->vte);
	tsm_screen_unref(locker->console);
	ev_eloop_unref(locker->eloop);
	free(locker->tty);
	free(locker->user);
	free(locker);
}

void locker_resize(struct locker *locker, unsigned int cols, unsigned int rows)
{
	tsm_screen_resize(locker->console, cols, rows);
	locker_draw(locker, locker->status);
}

struct tsm_screen *locker_get_screen(struct locker *locker)
{
	return locker->console;
}
