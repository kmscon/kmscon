/*
 * kmscon - KMS Console
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

#include <errno.h>
#include <paths.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include "conf.h"
#include "dbus.h"
#include "kmscon_conf.h"
#include "kmscon_seat.h"
#include "render/text.h"
#include "shl/eloop.h"
#include "shl/log.h"
#include "shl/module.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "kmscon"

struct kmscon_app {
	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;
	bool exiting;

	struct ev_eloop *eloop;

	struct conf_ctx *seat_ctx;
	struct kmscon_conf_t *seat_conf;
	struct kmscon_seat *seat;
};

static int app_seat_event(struct kmscon_seat *s, unsigned int event, void *data)
{
	struct kmscon_app *app = data;

	switch (event) {
	case KMSCON_SEAT_WAKE_UP:
		if (app->exiting)
			return -EBUSY;
		break;
	case KMSCON_SEAT_HUP:
		kmscon_seat_free(app->seat);
		app->seat = NULL;
		log_debug("seat HUP in default-mode; exiting...");
		ev_eloop_exit(app->eloop);
		break;
	}
	return 0;
}

static int setup_seat(struct kmscon_app *app)
{
	int ret;

	if (app->exiting)
		return -EBUSY;

	ret = kmscon_seat_new(&app->seat, app->conf_ctx, app->conf, app->eloop, app_seat_event,
			      app);
	if (ret) {
		log_error("cannot create seat object: %d", ret);
		return ret;
	}
	app->seat_ctx = kmscon_seat_get_conf(app->seat);
	app->seat_conf = conf_ctx_get_mem(app->seat_ctx);

	kmscon_seat_startup(app->seat);
	return 0;
}

static void destroy_seat(struct kmscon_app *app)
{
	kmscon_seat_free(app->seat);
	app->seat = NULL;
}

static void app_sig_generic(struct ev_eloop *eloop, struct signalfd_siginfo *info, void *data)
{
	struct kmscon_app *app = data;

	log_info("terminating due to caught signal %d", info->ssi_signo);
	ev_eloop_exit(app->eloop);
}

static void app_sig_ignore(struct ev_eloop *eloop, struct signalfd_siginfo *info, void *data) {}

static void destroy_app(struct kmscon_app *app)
{
	ev_eloop_unregister_signal_cb(app->eloop, SIGPIPE, app_sig_ignore, app);
	ev_eloop_unregister_signal_cb(app->eloop, SIGINT, app_sig_generic, app);
	ev_eloop_unregister_signal_cb(app->eloop, SIGTERM, app_sig_generic, app);
	ev_eloop_unref(app->eloop);
}

static int setup_app(struct kmscon_app *app)
{
	int ret;

	ret = ev_eloop_new(&app->eloop);
	if (ret) {
		log_error("cannot create eloop object: %d", ret);
		goto err_app;
	}

	ret = ev_eloop_register_signal_cb(app->eloop, SIGTERM, app_sig_generic, app);
	if (ret) {
		log_error("cannot register SIGTERM signal handler: %d", ret);
		goto err_app;
	}

	ret = ev_eloop_register_signal_cb(app->eloop, SIGINT, app_sig_generic, app);
	if (ret) {
		log_error("cannot register SIGINT signal handler: %d", ret);
		goto err_app;
	}

	ret = ev_eloop_register_signal_cb(app->eloop, SIGPIPE, app_sig_ignore, app);
	if (ret) {
		log_error("cannot register SIGPIPE signal handler: %d", ret);
		goto err_app;
	}

	return 0;

err_app:
	destroy_app(app);
	return ret;
}

int main(int argc, char **argv)
{
	int ret;
	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;
	struct kmscon_app app;

	ret = kmscon_conf_new(&conf_ctx);
	if (ret) {
		log_error("cannot create configuration: %d", ret);
		goto err_out;
	}
	conf = conf_ctx_get_mem(conf_ctx);

	ret = kmscon_conf_load_main(conf_ctx, argc, argv);
	if (ret)
		log_error("cannot load configuration: %d", ret);

	if (conf->exit) {
		kmscon_conf_free(conf_ctx);
		return 0;
	}
	dbus_set_xkb_env_from_locale();

	kmscon_load_modules();
	kmscon_font_register(&kmscon_font_8x16_ops);
	kmscon_text_register(&kmscon_text_bbulk_ops);
	video_register_drm2d();
	video_register_fbdev();

	memset(&app, 0, sizeof(app));
	app.conf_ctx = conf_ctx;
	app.conf = conf;

	ret = setup_app(&app);
	if (ret)
		goto err_unload;

	ret = setup_seat(&app);
	if (ret)
		goto err_unload_app;

	ev_eloop_run(app.eloop, -1);

	app.exiting = true;

	ret = 0;

	destroy_seat(&app);
err_unload_app:
	destroy_app(&app);
err_unload:
	kmscon_text_unregister(kmscon_text_bbulk_ops.name);
	kmscon_font_unregister(kmscon_font_8x16_ops.name);
	kmscon_unload_modules();
	kmscon_conf_free(conf_ctx);
err_out:
	if (ret)
		log_err("cannot initialize kmscon, errno %d: %s", ret, strerror(-ret));
	log_info("exiting");
	return -ret;
}
