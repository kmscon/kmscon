/*
 * uterm - Linux User-Space Terminal
 *
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
 * Virtual Terminals
 */

#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "input/input.h"
#include "shl/eloop.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "uterm_vt.h"
#include "uterm_vt_internal.h"

#define LOG_SUBSYSTEM "vt"

void vt_cb_activate(struct uterm_vt *vt)
{
	if (vt->active)
		return;

	vt->active = true;
	if (vt->cb.activate)
		vt->cb.activate(vt, vt->data);
}

int vt_cb_deactivate(struct uterm_vt *vt, bool force)
{
	if (!vt->active)
		return 0;

	vt->active = false;
	if (vt->cb.deactivate)
		return vt->cb.deactivate(vt, force, vt->data);
	return 0;
}

void vt_cb_hup(struct uterm_vt *vt)
{
	if (vt->cb.hup)
		vt->cb.hup(vt, vt->data);
}

SHL_EXPORT
struct uterm_vt *uterm_vt_allocate(struct ev_eloop *eloop, bool libseat, struct input *input,
				   const char *vt_name, struct uterm_vt_cb *cb, void *data)
{
	struct uterm_vt *vt = NULL;

	if (!eloop)
		return NULL;

	if (libseat)
		vt = uterm_vt_libseat_new(eloop, input, vt_name);

	if (!vt)
		vt = uterm_vt_real_new(eloop, input, vt_name);

	if (!vt)
		vt = uterm_vt_fake_new(eloop, input);

	if (!vt)
		return NULL;

	vt->cb = *cb;
	vt->data = data;

	input_ref(input);
	ev_eloop_ref(eloop);
	return vt;
}

SHL_EXPORT
void uterm_vt_deallocate(struct uterm_vt *vt)
{
	if (!vt)
		return;

	if (vt->ops->destroy)
		vt->ops->destroy(vt);

	input_unref(vt->input);
	ev_eloop_unref(vt->eloop);
	free(vt);
}

SHL_EXPORT
int uterm_vt_open_device(struct uterm_vt *vt, const char *device, int *fd_id)
{
	if (!vt)
		return -EINVAL;

	if (vt->ops->open_device)
		return vt->ops->open_device(vt, device, fd_id);

	return open(device, O_RDWR | O_CLOEXEC | O_NONBLOCK);
}

SHL_EXPORT
void uterm_vt_close_device(struct uterm_vt *vt, int fd, int fd_id)
{
	if (!vt)
		return;

	if (vt->ops->close_device)
		vt->ops->close_device(vt, fd, fd_id);
	else
		close(fd);
}

SHL_EXPORT
int uterm_vt_activate(struct uterm_vt *vt)
{
	if (!vt)
		return -EINVAL;

	return vt->ops->activate(vt);
}

SHL_EXPORT
int uterm_vt_deactivate(struct uterm_vt *vt)
{
	if (!vt)
		return -EINVAL;

	return vt->ops->deactivate(vt);
}

SHL_EXPORT
int uterm_vt_restore(struct uterm_vt *vt)
{
	if (!vt)
		return -EINVAL;

	if (vt->ops->restore)
		return vt->ops->restore(vt);

	return 0;
}

SHL_EXPORT
void uterm_vt_retry(struct uterm_vt *vt)
{
	if (!vt)
		return;

	if (vt->ops->retry)
		vt->ops->retry(vt);
}

SHL_EXPORT
unsigned int uterm_vt_get_num(struct uterm_vt *vt)
{
	if (vt && vt->ops->get_num)
		return vt->ops->get_num(vt);
	return 0;
}

SHL_EXPORT
void uterm_vt_bell(struct uterm_vt *vt)
{
	if (vt && vt->ops->bell)
		vt->ops->bell(vt);
}

SHL_EXPORT
const char *uterm_vt_get_name(struct uterm_vt *vt)
{
	if (vt && vt->ops->get_name)
		return vt->ops->get_name(vt);
	return "seat0";
}