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

#ifndef KMSCON_SEAT_H
#define KMSCON_SEAT_H

#include <stdbool.h>
#include "conf.h"
#include "kmscon_conf.h"
#include "shl/eloop.h"

struct kmscon_seat;
struct kmscon_session;
struct kmscon_video;

enum kmscon_seat_event {
	KMSCON_SEAT_WAKE_UP,
	KMSCON_SEAT_HUP,
};

typedef int (*kmscon_seat_cb_t)(struct kmscon_seat *seat, unsigned int event, void *data);

int kmscon_seat_new(struct kmscon_seat **out, struct conf_ctx *main_conf,
		    struct kmscon_conf_t *conf, struct ev_eloop *eloop, kmscon_seat_cb_t cb,
		    void *data);
void kmscon_seat_free(struct kmscon_seat *seat);
void kmscon_seat_startup(struct kmscon_seat *seat);

struct conf_ctx *kmscon_seat_get_conf(struct kmscon_seat *seat);

int kmscon_session_set_foreground(struct kmscon_session *sess);
int kmscon_session_set_background(struct kmscon_session *sess);
bool kmscon_session_get_foreground(struct kmscon_session *sess);

void kmscon_session_bell(struct kmscon_session *sess);
void kmscon_session_set_leds(struct kmscon_session *sess, unsigned int scroll_lock,
			     unsigned int num_lock, unsigned int caps_lock);

#endif /* KMSCON_SEAT_H */
