/*
 * Kmscon - Video Handling
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
 * Core Implementation of the video and display objects.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shl/dlist.h"
#include "shl/eloop.h"
#include "shl/hook.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "shl/module.h"
#include "shl/register.h"
#include "video.h"
#include "video_internal.h"

#define LOG_SUBSYSTEM "video"

static struct shl_register video_reg = SHL_REGISTER_INIT(video_reg);

static inline void video_destroy(void *data)
{
	const struct video_module *ops = data;

	shl_module_unref(ops->owner);
}

SHL_EXPORT
const char *dpms_to_name(enum display_dpms dpms)
{
	switch (dpms) {
	case DPMS_ON:
		return "ON";
	case DPMS_STANDBY:
		return "STANDBY";
	case DPMS_SUSPEND:
		return "SUSPEND";
	case DPMS_OFF:
		return "OFF";
	default:
		return "UNKNOWN";
	}
}

int display_new(struct display **out, const struct display_ops *ops, struct video *video,
		const char *name)
{
	struct display *disp;
	int ret;

	if (!out || !ops)
		return -EINVAL;

	disp = malloc(sizeof(*disp));
	if (!disp)
		return -ENOMEM;
	memset(disp, 0, sizeof(*disp));
	disp->name = strdup(name);
	disp->ref = 1;
	disp->ops = ops;
	log_info("new display %s %p", disp->name, disp);
	disp->video = video;

	ret = shl_hook_new(&disp->hook);
	if (ret)
		goto err_free;

	ret = disp->ops->init(disp);
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
void display_ref(struct display *disp)
{
	if (!disp || !disp->ref)
		return;

	++disp->ref;
}

SHL_EXPORT
void display_unref(struct display *disp)
{
	if (!disp || !disp->ref || --disp->ref)
		return;

	log_info("free display %s %p", disp->name, disp);

	disp->ops->destroy(disp);
	shl_hook_free(disp->hook);
	free(disp->name);
	free(disp);
}

SHL_EXPORT
int display_bind(struct display *disp)
{
	if (!disp || !disp->video)
		return -EINVAL;

	shl_dlist_link_tail(&disp->video->displays, &disp->list);
	display_ref(disp);

	return 0;
}

SHL_EXPORT
void display_ready(struct display *disp)
{
	if (!disp || !disp->video || disp->flags & DISPLAY_INUSE)
		return;

	disp->flags |= DISPLAY_INUSE;
	disp->video->cb->new_disp(disp->video->cb_data, disp);
}

SHL_EXPORT
void display_unbind(struct display *disp)
{
	if (!disp || !disp->video)
		return;
	if (disp->flags & DISPLAY_INUSE)
		disp->video->cb->remove_disp(disp->video->cb_data, disp);
	shl_dlist_unlink(&disp->list);
	display_unref(disp);
}

SHL_EXPORT
bool display_is_drm(struct display *disp)
{
	return (disp->flags & DISPLAY_DITHERING) == 0;
}

SHL_EXPORT
bool display_has_opengl(struct display *disp)
{
	return (disp->flags & DISPLAY_OPENGL) != 0;
}

SHL_EXPORT
bool display_supports_damage(struct display *disp)
{
	return (disp->flags & DISPLAY_DAMAGE) != 0;
}

SHL_EXPORT
const char *display_backend_name(struct display *disp)
{
	if (disp && disp->video && disp->video->mod)
		return disp->video->mod->name;
	return "Unknown";
}

SHL_EXPORT
const char *display_name(struct display *disp)
{
	if (disp && disp->name)
		return disp->name;
	return "Unknown";
}

SHL_EXPORT
struct video *display_video(struct display *disp)
{
	if (!disp)
		return NULL;
	return disp->video;
}

SHL_EXPORT
int display_register_pageflip(struct display *disp, display_pageflip_cb cb, void *data)
{
	if (!disp)
		return -EINVAL;

	return shl_hook_add_cast(disp->hook, cb, data, false);
}

SHL_EXPORT
void display_unregister_pageflip(struct display *disp, display_pageflip_cb cb, void *data)
{
	if (!disp)
		return;

	shl_hook_rm_cast(disp->hook, cb, data);
}

SHL_EXPORT
unsigned int display_get_width(struct display *disp)
{
	if (!disp)
		return 0;

	return disp->width;
}

SHL_EXPORT
unsigned int display_get_height(struct display *disp)
{
	if (!disp)
		return 0;

	return disp->height;
}

SHL_EXPORT
int display_get_state(struct display *disp)
{
	if (!disp || !disp->video)
		return DISPLAY_GONE;
	if (disp->flags & DISPLAY_ONLINE) {
		if (disp->video->flags & VIDEO_AWAKE)
			return DISPLAY_ACTIVE;
		return DISPLAY_INACTIVE;
	}
	return DISPLAY_ASLEEP;
}

SHL_EXPORT
int display_set_dpms(struct display *disp, enum display_dpms dpms)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;
	if (disp->ops->set_dpms)
		return disp->ops->set_dpms(disp, dpms);
	return 0;
}

SHL_EXPORT
enum display_dpms display_get_dpms(const struct display *disp)
{
	if (!disp || !disp->video)
		return DPMS_OFF;

	return disp->dpms;
}

SHL_EXPORT
int display_use(struct display *disp)
{
	if (!disp || !display_is_online(disp) || !disp->ops->use)
		return -EINVAL;

	return disp->ops->use(disp);
}

SHL_EXPORT
int display_swap(struct display *disp)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;
	if (disp->ops->swap)
		disp->ops->swap(disp);
	return 0;
}

SHL_EXPORT
bool display_is_swapping(struct display *disp)
{
	if (!disp || !disp->ops->is_swapping)
		return false;

	return disp->ops->is_swapping(disp);
}

SHL_EXPORT
int display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video) || !disp->ops->clear)
		return -EINVAL;

	return disp->ops->clear(disp, r, g, b);
}

SHL_EXPORT
int display_blend(struct display *disp, const struct video_blend_req *req)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	if (disp->ops->blend)
		return disp->ops->blend(disp, req);
	return -EOPNOTSUPP;
}

SHL_EXPORT
void display_set_need_redraw(struct display *disp)
{
	if (!disp || !display_is_online(disp))
		return;

	disp->flags |= DISPLAY_NEED_REDRAW;
}

SHL_EXPORT
bool display_need_redraw(struct display *disp)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return false;

	return (disp->flags & DISPLAY_NEED_REDRAW) != 0;
}

SHL_EXPORT
int display_setup_cursor(struct display *disp, const uint32_t *pixels, unsigned int width,
			 unsigned int height, int hot_x, int hot_y)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	if (disp->ops->setup_cursor)
		return disp->ops->setup_cursor(disp, pixels, width, height, hot_x, hot_y);
	return -EOPNOTSUPP;
}

SHL_EXPORT
void display_destroy_cursor(struct display *disp)
{
	if (disp && disp->ops->destroy_cursor)
		disp->ops->destroy_cursor(disp);
}

SHL_EXPORT
int display_show_cursor(struct display *disp, int32_t x, int32_t y)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	if (disp->ops->show_cursor)
		return disp->ops->show_cursor(disp, x, y);
	return -EOPNOTSUPP;
}

SHL_EXPORT
int display_hide_cursor(struct display *disp)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	if (disp->ops->hide_cursor)
		return disp->ops->hide_cursor(disp);
	return -EOPNOTSUPP;
}

SHL_EXPORT
void display_set_cursor_offset(struct display *disp, int32_t x, int32_t y)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return;

	if (disp->ops->set_cursor_offset)
		disp->ops->set_cursor_offset(disp, x, y);
}

SHL_EXPORT
void display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return;

	if (disp->ops->set_damage)
		disp->ops->set_damage(disp, n_rect, damages);
}

SHL_EXPORT
bool display_has_damage(struct display *disp)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return false;

	if (disp->ops->has_damage)
		return disp->ops->has_damage(disp);
	return false;
}

SHL_EXPORT
int video_new(struct video **out, struct ev_eloop *eloop, int fd, const char *backend,
	      struct video_cb *cb, void *data, unsigned int desired_width,
	      unsigned int desired_height, bool use_original)
{
	struct shl_register_record *record;
	const char *name = backend ? backend : "<default>";
	struct video *video;
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

	video->record = record;
	video->mod = record->data;
	video->cb = cb;
	video->cb_data = data;

	video->eloop = eloop;
	shl_dlist_init(&video->displays);

	ret = video->mod->ops.init(video, fd);
	if (ret)
		goto err_free;

	video->desired_width = desired_width;
	video->desired_height = desired_height;

	ev_eloop_ref(video->eloop);
	log_info("new device %p", video);
	*out = video;
	return 0;

err_free:
	free(video);
err_unref:
	shl_register_record_unref(record);
	return ret;
}

SHL_EXPORT
void video_ref(struct video *video)
{
	if (!video || !video->ref)
		return;

	++video->ref;
}

SHL_EXPORT
void video_unref(struct video *video)
{
	struct display *disp;

	if (!video || !video->ref || --video->ref)
		return;

	log_info("free device %p", video);

	while (!shl_dlist_empty(&video->displays)) {
		disp = shl_dlist_entry(video->displays.prev, struct display, list);
		display_unbind(disp);
	}

	video->mod->ops.destroy(video);
	ev_eloop_unref(video->eloop);
	shl_register_record_unref(video->record);
	free(video);
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
int video_register(const struct video_module *ops)
{
	int ret;

	if (!ops)
		return -EINVAL;

	log_debug("register video backend %s", ops->name);

	ret = shl_register_add_cb(&video_reg, ops->name, (void *)ops, video_destroy);
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
void video_unregister(const char *name)
{
	log_debug("unregister backend %s", name);
	shl_register_remove(&video_reg, name);
}

SHL_EXPORT
void video_sleep(struct video *video)
{
	if (!video || !video_is_awake(video))
		return;

	log_debug("go asleep");
	video->flags &= ~VIDEO_AWAKE;
	if (video->mod->ops.sleep)
		video->mod->ops.sleep(video);
}

SHL_EXPORT
int video_wake_up(struct video *video)
{
	int ret = 0;

	if (!video)
		return -EINVAL;
	if (video_is_awake(video))
		return 0;

	log_debug("wake up");

	if (video->mod->ops.wake_up)
		ret = video->mod->ops.wake_up(video);
	if (ret) {
		video->flags &= ~VIDEO_AWAKE;
		return ret;
	}

	video->flags |= VIDEO_AWAKE;
	return 0;
}

SHL_EXPORT
bool video_is_awake(struct video *video)
{
	return video && (video->flags & VIDEO_AWAKE);
}

SHL_EXPORT
void video_poll(struct video *video)
{
	if (video && video->mod->ops.poll)
		video->mod->ops.poll(video);
}
