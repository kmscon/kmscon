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
 * Fake VT:
 * For systems without CONFIG_VT or for all seats that have no real VTs (which
 * is all seats except seat0), we support a fake-VT mechanism. This mechanism is
 * only used for debugging and should not be used in production.
 *
 * Fake-VTs react on a key-press and activate themselves if not active. If they
 * are already active, they deactivate themselves. To switch from one fake-VT to
 * another, you first need to deactivate the current fake-VT and then activate
 * the new fake-VT. This also means that you must use different hotkeys for each
 * fake-VT.
 * This is a very fragile infrastructure and should only be used for debugging.
 *
 * To avoid this bad situation, you simply activate a fake-VT during startup
 * with uterm_vt_activate() and then do not use the hotkeys at all. This assumes
 * that the fake-VT is the only application on this seat.
 *
 * If you use multiple fake-VTs on a seat without real-VTs, you should really
 * use some other daemon that handles VT-switches. Otherwise, there is no sane
 * way to communicate this between the fake-VTs. So please use fake-VTs only for
 * debugging or if they are the only session on their seat.
 */

#include <string.h>

#include "shl_log.h"
#include "shl_misc.h"
#include "uterm_vt.h"
#include "uterm_vt_internal.h"

static int fake_activate(struct uterm_vt *vt)
{
	log_debug("activating fake VT due to user request");
	vt_call_activate(vt, 0);
	return 0;
}

static int fake_deactivate(struct uterm_vt *vt)
{
	log_debug("deactivating fake VT due to user request");
	return vt_call_deactivate(vt, 0, false);
}

static void fake_input(struct uterm_input *input, struct uterm_input_key_event *ev, void *data)
{
	struct uterm_vt *vt = data;

	if (ev->handled)
		return;

	if (SHL_HAS_BITS(ev->mods, SHL_CONTROL_MASK | SHL_LOGO_MASK) &&
	    ev->keysyms[0] == XKB_KEY_F12) {
		ev->handled = true;
		if (vt->active) {
			log_debug("deactivating fake VT due to user input");
			vt_call_deactivate(vt, 0, false);
		} else {
			log_debug("activating fake VT due to user input");
			vt_call_activate(vt, 0);
		}
	}
}

static void fake_destroy(struct uterm_vt *vt)
{
	vt_call_deactivate(vt, 0, true);
	uterm_input_sleep(vt->input);
}

static const struct uterm_vt_ops fake_ops = {
	.destroy = fake_destroy,
	.activate = fake_activate,
	.deactivate = fake_deactivate,
};

struct uterm_vt *uterm_vt_fake_new(struct uterm_vt_master *vtm, struct uterm_input *input,
				   uterm_vt_cb cb, void *data)
{
	struct uterm_vt *vt;
	int ret;

	vt = malloc(sizeof(*vt));
	if (!vt)
		return NULL;
	memset(vt, 0, sizeof(*vt));
	vt->vtm = vtm;
	vt->input = input;
	vt->cb = cb;
	vt->data = data;
	vt->ops = &fake_ops;

	ret = uterm_input_register_key_cb(input, fake_input, vt);
	if (ret)
		goto err_free;

	uterm_input_wake_up(vt->input);

	return vt;

err_free:
	free(vt);
	return NULL;
}
