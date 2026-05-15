/*
 * uterm - Linux User-Space Terminal VT API
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * Virtual Terminals
 * Virtual terminals allow controlling multiple virtual terminals on one real
 * terminal. It is multi-seat capable and fully asynchronous.
 */

#ifndef UTERM_UTERM_VT_H
#define UTERM_UTERM_VT_H

#include <inttypes.h>
#include <stdbool.h>
#include "input/input.h"
#include "shl/eloop.h"

struct uterm_vt;

typedef void (*uterm_vt_activate_cb)(struct uterm_vt *vt, void *data);
typedef int (*uterm_vt_deactivate_cb)(struct uterm_vt *vt, bool force, void *data);
typedef void (*uterm_vt_hup_cb)(struct uterm_vt *vt, void *data);

struct uterm_vt_cb {
	uterm_vt_activate_cb activate;
	uterm_vt_deactivate_cb deactivate;
	uterm_vt_hup_cb hup;
};

struct uterm_vt *uterm_vt_allocate(struct ev_eloop *eloop, bool libseat, struct uterm_input *input,
				   const char *vt_name, struct uterm_vt_cb *cb, void *data);
void uterm_vt_deallocate(struct uterm_vt *vt);

int uterm_vt_open_device(struct uterm_vt *vt, const char *device, int *fd_id);
void uterm_vt_close_device(struct uterm_vt *vt, int fd, int fd_id);

int uterm_vt_activate(struct uterm_vt *vt);
int uterm_vt_deactivate(struct uterm_vt *vt);
void uterm_vt_retry(struct uterm_vt *vt);
unsigned int uterm_vt_get_num(struct uterm_vt *vt);
void uterm_vt_bell(struct uterm_vt *vt);
int uterm_vt_restore(struct uterm_vt *vt);
const char *uterm_vt_get_name(struct uterm_vt *vt);

#endif /* UTERM_UTERM_VT_H */
