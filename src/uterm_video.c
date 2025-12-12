/*
 * uterm - Linux User-Space Terminal
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
 * Video Control
 * Core Implementation of the uterm_video and uterm_display objects.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "eloop.h"
#include "shl_dlist.h"
#include "shl_hook.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "shl_module.h"
#include "shl_register.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "video"

static struct shl_register video_reg = SHL_REGISTER_INIT(video_reg);

static inline void uterm_video_destroy(void *data)
{
	const struct uterm_video_module *ops = data;

	shl_module_unref(ops->owner);
}

SHL_EXPORT
const char *uterm_dpms_to_name(int dpms)
{
	switch (dpms) {
	case UTERM_DPMS_ON:
		return "ON";
	case UTERM_DPMS_STANDBY:
		return "STANDBY";
	case UTERM_DPMS_SUSPEND:
		return "SUSPEND";
	case UTERM_DPMS_OFF:
		return "OFF";
	default:
		return "UNKNOWN";
	}
}

int display_new(struct uterm_display **out, const struct display_ops *ops)
{
	struct uterm_display *disp;
	int ret;

	if (!out || !ops)
		return -EINVAL;

	disp = malloc(sizeof(*disp));
	if (!disp)
		return -ENOMEM;
	memset(disp, 0, sizeof(*disp));
	disp->ref = 1;
	disp->ops = ops;
	log_info("new display %p", disp);

	ret = shl_hook_new(&disp->hook);
	if (ret)
		goto err_free;

	ret = VIDEO_CALL(disp->ops->init, 0, disp);
	if (ret)
		goto err_hook;

	*out = disp;
	return 0;

err_hook:
	shl_hook_free(disp->hook);
err_free:
	free(disp);
	return ret;
}

SHL_EXPORT
void uterm_display_ref(struct uterm_display *disp)
{
	if (!disp || !disp->ref)
		return;

	++disp->ref;
}

SHL_EXPORT
void uterm_display_unref(struct uterm_display *disp)
{
	if (!disp || !disp->ref || --disp->ref)
		return;

	log_info("free display %p", disp);

	VIDEO_CALL(disp->ops->destroy, 0, disp);
	shl_hook_free(disp->hook);
	free(disp);
}

SHL_EXPORT
int uterm_display_bind(struct uterm_display *disp, struct uterm_video *video)
{
	if (!disp || !video || disp->video)
		return -EINVAL;

	shl_dlist_link_tail(&video->displays, &disp->list);
	disp->video = video;
	uterm_display_ref(disp);
	VIDEO_CB(disp->video, disp, UTERM_NEW);

	return 0;
}

SHL_EXPORT
void uterm_display_unbind(struct uterm_display *disp)
{
	if (!disp || !disp->video)
		return;

	VIDEO_CB(disp->video, disp, UTERM_GONE);
	uterm_display_deactivate(disp);
	disp->video = NULL;
	shl_dlist_unlink(&disp->list);
	uterm_display_unref(disp);
}

SHL_EXPORT
bool uterm_display_is_drm(struct uterm_display *disp)
{
	return (disp->flags & DISPLAY_DITHERING) == 0;
}

SHL_EXPORT
bool uterm_display_has_opengl(struct uterm_display *disp)
{
	return (disp->flags & DISPLAY_OPENGL) != 0;
}

SHL_EXPORT
const char *uterm_display_backend_name(struct uterm_display *disp)
{
	if (disp && disp->video && disp->video->mod)
		return disp->video->mod->name;
	return "Unknown";
}

SHL_EXPORT
struct uterm_display *uterm_display_next(struct uterm_display *disp)
{
	if (!disp || !disp->video || disp->list.next == &disp->video->displays)
		return NULL;

	return shl_dlist_entry(disp->list.next, struct uterm_display, list);
}

SHL_EXPORT
int uterm_display_register_cb(struct uterm_display *disp, uterm_display_cb cb, void *data)
{
	if (!disp)
		return -EINVAL;

	return shl_hook_add_cast(disp->hook, cb, data, false);
}

SHL_EXPORT
void uterm_display_unregister_cb(struct uterm_display *disp, uterm_display_cb cb, void *data)
{
	if (!disp)
		return;

	shl_hook_rm_cast(disp->hook, cb, data);
}

SHL_EXPORT
unsigned int uterm_display_get_width(struct uterm_display *disp)
{
	if (!disp)
		return 0;

	return disp->width;
}

SHL_EXPORT
unsigned int uterm_display_get_height(struct uterm_display *disp)
{
	if (!disp)
		return 0;

	return disp->height;
}

SHL_EXPORT
int uterm_display_get_state(struct uterm_display *disp)
{
	if (!disp || !disp->video)
		return UTERM_DISPLAY_GONE;
	if (disp->flags & DISPLAY_ONLINE) {
		if (disp->video->flags & VIDEO_AWAKE)
			return UTERM_DISPLAY_ACTIVE;
		return UTERM_DISPLAY_INACTIVE;
	}
	return UTERM_DISPLAY_ASLEEP;
}

SHL_EXPORT
int uterm_display_activate(struct uterm_display *disp)
{
	if (!disp || !disp->video || display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->activate, 0, disp);
}

SHL_EXPORT
void uterm_display_deactivate(struct uterm_display *disp)
{
	if (!disp || !display_is_online(disp))
		return;

	VIDEO_CALL(disp->ops->deactivate, 0, disp);
}

SHL_EXPORT
int uterm_display_set_dpms(struct uterm_display *disp, int state)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->set_dpms, 0, disp, state);
}

SHL_EXPORT
int uterm_display_get_dpms(const struct uterm_display *disp)
{
	if (!disp || !disp->video)
		return UTERM_DPMS_OFF;

	return disp->dpms;
}

SHL_EXPORT
int uterm_display_use(struct uterm_display *disp)
{
	if (!disp || !display_is_online(disp))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->use, -EOPNOTSUPP, disp);
}

SHL_EXPORT
int uterm_display_swap(struct uterm_display *disp, bool immediate)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->swap, 0, disp, immediate);
}

SHL_EXPORT
bool uterm_display_is_swapping(struct uterm_display *disp)
{
	if (!disp)
		return false;

	return VIDEO_CALL(disp->ops->is_swapping, 0, disp);
}

SHL_EXPORT
int uterm_display_fill(struct uterm_display *disp, uint8_t r, uint8_t g, uint8_t b, unsigned int x,
		       unsigned int y, unsigned int width, unsigned int height)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->fill, -EOPNOTSUPP, disp, r, g, b, x, y, width, height);
}

SHL_EXPORT
int uterm_display_fake_blendv(struct uterm_display *disp, const struct uterm_video_blend_req *req,
			      size_t num)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->fake_blendv, -EOPNOTSUPP, disp, req, num);
}

SHL_EXPORT
int uterm_video_new(struct uterm_video **out, struct ev_eloop *eloop, const char *node,
		    const char *backend, unsigned int desired_width, unsigned int desired_height,
		    bool use_original)
{
	struct shl_register_record *record;
	const char *name = backend ? backend : "<default>";
	struct uterm_video *video;
	int ret;

	if (!out || !eloop)
		return -EINVAL;

	if (backend)
		record = shl_register_find(&video_reg, backend);
	else
		record = shl_register_first(&video_reg);

	if (!record) {
		log_error("requested backend '%s' not found", name);
		return -ENOENT;
	}

	video = malloc(sizeof(*video));
	if (!video) {
		ret = -ENOMEM;
		goto err_unref;
	}
	memset(video, 0, sizeof(*video));
	video->ref = 1;

	video->mod = record->data;

	video->eloop = eloop;
	shl_dlist_init(&video->displays);

	ret = shl_hook_new(&video->hook);
	if (ret)
		goto err_free;

	ret = VIDEO_CALL(video->mod->ops.init, 0, video, node);
	if (ret)
		goto err_hook;

	video->desired_width = desired_width;
	video->desired_height = desired_height;

	ev_eloop_ref(video->eloop);
	log_info("new device %p", video);
	*out = video;
	return 0;

err_hook:
	shl_hook_free(video->hook);
err_free:
	free(video);
err_unref:
	shl_register_record_unref(record);
	return ret;
}

SHL_EXPORT
void uterm_video_ref(struct uterm_video *video)
{
	if (!video || !video->ref)
		return;

	++video->ref;
}

SHL_EXPORT
void uterm_video_unref(struct uterm_video *video)
{
	struct uterm_display *disp;

	if (!video || !video->ref || --video->ref)
		return;

	log_info("free device %p", video);

	while (!shl_dlist_empty(&video->displays)) {
		disp = shl_dlist_entry(video->displays.prev, struct uterm_display, list);
		uterm_display_unbind(disp);
	}

	VIDEO_CALL(video->mod->ops.destroy, 0, video);
	shl_hook_free(video->hook);
	ev_eloop_unref(video->eloop);
	free(video);
}

SHL_EXPORT
struct uterm_display *uterm_video_get_displays(struct uterm_video *video)
{
	if (!video || shl_dlist_empty(&video->displays))
		return NULL;

	return shl_dlist_entry(video->displays.next, struct uterm_display, list);
}

SHL_EXPORT
int uterm_video_register_cb(struct uterm_video *video, uterm_video_cb cb, void *data)
{
	if (!video || !cb)
		return -EINVAL;

	return shl_hook_add_cast(video->hook, cb, data, false);
}

SHL_EXPORT
void uterm_video_unregister_cb(struct uterm_video *video, uterm_video_cb cb, void *data)
{
	if (!video || !cb)
		return;

	shl_hook_rm_cast(video->hook, cb, data);
}

/**
 * kmscon_video_register:
 * @ops: a video module.
 *
 * This register a new video backend with operations set to @ops. The name
 * @ops->name must be valid.
 *
 * Returns: 0 on success, negative error code on failure
 */
SHL_EXPORT
int uterm_video_register(const struct uterm_video_module *ops)
{
	int ret;

	if (!ops)
		return -EINVAL;

	log_debug("register video backend %s", ops->name);

	ret = shl_register_add_cb(&video_reg, ops->name, (void *)ops, uterm_video_destroy);
	if (ret) {
		log_error("cannot register video backend %s: %d", ops->name, ret);
		return ret;
	}

	shl_module_ref(ops->owner);
	return 0;
}

/**
 * kmscon_video_unregister:
 * @name: Name of backend
 *
 * This unregisters the video-backend that is registered with name @name. If
 * @name is not found, nothing is done.
 */
SHL_EXPORT
void uterm_video_unregister(const char *name)
{
	log_debug("unregister backend %s", name);
	shl_register_remove(&video_reg, name);
}

SHL_EXPORT
void uterm_video_sleep(struct uterm_video *video)
{
	if (!video || !video_is_awake(video))
		return;

	log_debug("go asleep");

	VIDEO_CB(video, NULL, UTERM_SLEEP);
	video->flags &= ~VIDEO_AWAKE;
	VIDEO_CALL(video->mod->ops.sleep, 0, video);
}

SHL_EXPORT
int uterm_video_wake_up(struct uterm_video *video)
{
	int ret;

	if (!video)
		return -EINVAL;
	if (video_is_awake(video))
		return 0;

	log_debug("wake up");

	ret = VIDEO_CALL(video->mod->ops.wake_up, 0, video);
	if (ret) {
		video->flags &= ~VIDEO_AWAKE;
		return ret;
	}

	video->flags |= VIDEO_AWAKE;
	VIDEO_CB(video, NULL, UTERM_WAKE_UP);
	return 0;
}

SHL_EXPORT
bool uterm_video_is_awake(struct uterm_video *video)
{
	return video && video_is_awake(video);
}

SHL_EXPORT
void uterm_video_poll(struct uterm_video *video)
{
	if (!video)
		return;

	VIDEO_CALL(video->mod->ops.poll, 0, video);
}
