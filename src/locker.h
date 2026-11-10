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

/*
 * Terminal locker
 * A locked terminal shows a password prompt on a separate screen, while the
 * real console keeps running behind it. The typed password is checked with
 * the kmscon-lock PAM service.
 */

#ifndef KMSCON_LOCKER_H
#define KMSCON_LOCKER_H

struct ev_eloop;
struct input_key_event;
struct kmscon_conf_t;
struct locker;
struct tsm_screen;

enum locker_event {
	LOCKER_REDRAW,	 /* the screen content changed */
	LOCKER_UNLOCKED, /* the right password was entered */
};

/* The locker may be freed from this callback */
typedef void (*locker_cb)(struct locker *locker, enum locker_event ev, void *data);

struct locker *locker_new(struct ev_eloop *eloop, struct kmscon_conf_t *conf, const char *user,
			  const char *tty, unsigned int cols, unsigned int rows, locker_cb cb,
			  void *data);
void locker_free(struct locker *locker);
void locker_input(struct locker *locker, struct input_key_event *ev);
void locker_resize(struct locker *locker, unsigned int cols, unsigned int rows);
struct tsm_screen *locker_get_screen(struct locker *locker);

#endif /* KMSCON_LOCKER_H */
