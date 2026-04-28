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

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "eloop.h"
#include "shl_dlist.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "uterm_input.h"
#include "uterm_vt.h"
#include "uterm_vt_internal.h"

#define LOG_SUBSYSTEM "vt"

static int vt_call(struct uterm_vt *vt, unsigned int event, int target, bool force)
{
	int ret;
	struct uterm_vt_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.action = event;
	ev.target = target;
	if (force)
		ev.flags |= UTERM_VT_FORCE;

	switch (event) {
	case UTERM_VT_ACTIVATE:
		if (vt->active)
			return 0;
		if (!vt->cb)
			break;

		ret = vt->cb(vt, &ev, vt->data);
		if (ret)
			log_warning("vt event handler returned %d instead of 0 on activation", ret);
		break;
	case UTERM_VT_DEACTIVATE:
		if (!vt->active)
			return 0;
		if (!vt->cb)
			break;

		ret = vt->cb(vt, &ev, vt->data);
		if (ret) {
			if (force)
				log_warning("vt event handler returned %d instead of 0 on forced "
					    "deactivation",
					    ret);
			else
				return ret;
		}
		break;
	default:
		return -EINVAL;
	}

	vt->active = !vt->active;
	return 0;
}

void vt_call_activate(struct uterm_vt *vt, int target)
{
	vt_call(vt, UTERM_VT_ACTIVATE, target, false);
}

int vt_call_deactivate(struct uterm_vt *vt, int target, bool force)
{
	return vt_call(vt, UTERM_VT_DEACTIVATE, target, force);
}

SHL_EXPORT
int uterm_vt_allocate(struct uterm_vt_master *vtm, struct uterm_vt **out, bool listen,
		      const char *seat, struct uterm_input *input, const char *vt_name,
		      uterm_vt_cb cb, void *data)
{
	struct uterm_vt *vt = NULL;

	if (!vtm || !out)
		return -EINVAL;
	if (!seat)
		seat = "seat0";

	if (!listen)
		vt = uterm_vt_real_new(vtm, input, vt_name, cb, data);

	if (!vt)
		vt = uterm_vt_fake_new(vtm, input, cb, data);

	if (!vt)
		return -EFAULT;

	uterm_input_ref(input);
	shl_dlist_link(&vtm->vts, &vt->list);

	*out = vt;
	return 0;
}

SHL_EXPORT
void uterm_vt_deallocate(struct uterm_vt *vt)
{
	if (!vt || !vt->vtm)
		return;

	if (vt->ops->destroy)
		vt->ops->destroy(vt);

	shl_dlist_unlink(&vt->list);
	uterm_input_unref(vt->input);
	vt->vtm = NULL;
	free(vt);
}

SHL_EXPORT
int uterm_vt_activate(struct uterm_vt *vt)
{
	if (!vt || !vt->vtm)
		return -EINVAL;

	return vt->ops->activate(vt);
}

SHL_EXPORT
int uterm_vt_deactivate(struct uterm_vt *vt)
{
	if (!vt || !vt->vtm)
		return -EINVAL;

	return vt->ops->deactivate(vt);
}

SHL_EXPORT
int uterm_vt_restore(struct uterm_vt *vt)
{
	if (!vt || !vt->vtm)
		return -EINVAL;

	if (vt->ops->restore)
		return vt->ops->restore(vt);

	return 0;
}

SHL_EXPORT
void uterm_vt_retry(struct uterm_vt *vt)
{
	if (!vt || !vt->vtm)
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
int uterm_vt_master_new(struct uterm_vt_master **out, struct ev_eloop *eloop)
{
	struct uterm_vt_master *vtm;

	if (!out || !eloop)
		return -EINVAL;

	vtm = malloc(sizeof(*vtm));
	if (!vtm)
		return -ENOMEM;
	memset(vtm, 0, sizeof(*vtm));
	vtm->ref = 1;
	vtm->eloop = eloop;
	shl_dlist_init(&vtm->vts);

	ev_eloop_ref(vtm->eloop);
	*out = vtm;
	return 0;
}

SHL_EXPORT
void uterm_vt_master_ref(struct uterm_vt_master *vtm)
{
	if (!vtm || !vtm->ref)
		return;

	++vtm->ref;
}

/* Drops a reference to the VT-master. If the reference drops to 0, all
 * allocated VTs are deallocated and the VT-master is destroyed. */
SHL_EXPORT
void uterm_vt_master_unref(struct uterm_vt_master *vtm)
{
	struct uterm_vt *vt;

	if (!vtm || !vtm->ref || --vtm->ref)
		return;

	while (vtm->vts.next != &vtm->vts) {
		vt = shl_dlist_entry(vtm->vts.next, struct uterm_vt, list);
		uterm_vt_deallocate(vt);
	}

	ev_eloop_unref(vtm->eloop);
	free(vtm);
}

/* Calls uterm_vt_deactivate() on all allocated VTs on this master. Returns
 * number of VTs that returned -EINPROGRESS or a negative error code on failure.
 * See uterm_vt_deactivate() for information. */
SHL_EXPORT
int uterm_vt_master_deactivate_all(struct uterm_vt_master *vtm)
{
	struct uterm_vt *vt;
	struct shl_dlist *iter;
	int ret, res = 0;
	unsigned int in_progress = 0;

	if (!vtm)
		return -EINVAL;

	shl_dlist_for_each(iter, &vtm->vts)
	{
		vt = shl_dlist_entry(iter, struct uterm_vt, list);
		ret = uterm_vt_deactivate(vt);
		if (ret == -EINPROGRESS)
			in_progress++;
		else if (ret)
			res = ret;
	}

	if (in_progress)
		return in_progress;

	return res;
}
