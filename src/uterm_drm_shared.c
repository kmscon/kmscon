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
 * DRM shared functions
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "shl_dlist.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "shl_timer.h"
#include "uterm_drm_shared_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "drm_shared"

static uint32_t get_property_id(int fd, drmModeObjectPropertiesPtr props, const char *name)
{
	drmModePropertyPtr prop;
	uint32_t id;
	int j;

	for (j = 0; j < props->count_props; j++) {
		prop = drmModeGetProperty(fd, props->props[j]);
		id = prop->prop_id;
		if (!strcmp(prop->name, name)) {
			drmModeFreeProperty(prop);
			return id;
		}
	}
	log_err("drm property %s not found", name);
	return 0;
}

static uint64_t get_property_value(int fd, drmModeObjectPropertiesPtr props, const char *name)
{
	drmModePropertyPtr prop;
	uint64_t value;
	int j;

	for (j = 0; j < props->count_props; j++) {
		prop = drmModeGetProperty(fd, props->props[j]);
		value = props->prop_values[j];
		if (!strcmp(prop->name, name)) {
			drmModeFreeProperty(prop);
			return value;
		}
	}
	log_err("drm property %s not found", name);
	return 0;
}

static const char *drm_mode_prop_name(uint32_t type)
{
	switch (type) {
	case DRM_MODE_OBJECT_CONNECTOR:
		return "connector";
	case DRM_MODE_OBJECT_PLANE:
		return "plane";
	case DRM_MODE_OBJECT_CRTC:
		return "CRTC";
	default:
		return "unknown type";
	}
}

static void modeset_get_object_properties(int fd, struct drm_object *obj, uint32_t type)
{
	unsigned int i;

	obj->props = drmModeObjectGetProperties(fd, obj->id, type);
	if (!obj->props) {
		log_err("cannot get %s %d properties: %s\n", drm_mode_prop_name(type), obj->id,
			strerror(errno));
		return;
	}

	obj->props_info = calloc(obj->props->count_props, sizeof(obj->props_info));
	for (i = 0; i < obj->props->count_props; i++)
		obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
}

static int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj, const char *name,
				   uint64_t value)
{
	int i;
	uint32_t prop_id = 0;

	for (i = 0; i < obj->props->count_props; i++) {
		if (!strcmp(obj->props_info[i]->name, name)) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id == 0) {
		log_err("no object property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj->id, prop_id, value);
}

static bool is_crtc_in_use(struct uterm_video *video, uint32_t crtc_id)
{
	struct shl_dlist *iter;
	struct uterm_display *disp;
	struct uterm_drm_display *ddrm;

	shl_dlist_for_each(iter, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		ddrm = disp->data;
		if (ddrm->crtc.id == crtc_id)
			return true;
	}
	return false;
}

static int get_crtc_index(drmModeRes *res, uint32_t crtc_id)
{
	int i;
	for (i = 0; i < res->count_crtcs; ++i)
		if (res->crtcs[i] == crtc_id)
			return i;

	log_err("Can't find CRTC index for CRTC %d", crtc_id);
	return 0;
}

static int modeset_find_crtc(struct uterm_video *video, int fd, drmModeRes *res,
			     drmModeConnector *conn, struct uterm_drm_display *ddrm)
{
	drmModeEncoder *enc;
	unsigned int i, j;

	/* first try the currently connected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id && !is_crtc_in_use(video, enc->crtc_id)) {
			ddrm->crtc.id = enc->crtc_id;
			drmModeFreeEncoder(enc);
			ddrm->crtc_index = get_crtc_index(res, ddrm->crtc.id);
			return 0;
		}
		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC.
	 */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			log_err("cannot retrieve encoder %u:%u (%d): %m\n", i, conn->encoders[i],
				errno);
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other output already uses this CRTC */
			if (is_crtc_in_use(video, res->crtcs[j]))
				continue;

			log_info("crtc %u found for encoder %u, will need full modeset\n",
				 res->crtcs[j], conn->encoders[i]);
			drmModeFreeEncoder(enc);
			ddrm->crtc.id = res->crtcs[j];
			ddrm->crtc_index = j;
			return 0;
		}
		drmModeFreeEncoder(enc);
	}
	log_err("cannot find suitable crtc for connector %u\n", conn->connector_id);
	return -ENOENT;
}

static int modeset_find_plane(int fd, struct uterm_drm_display *ddrm)
{
	drmModePlaneResPtr plane_res;
	bool found_primary = false;
	int i, ret = -EINVAL;

	plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res) {
		log_err("drmModeGetPlaneResources failed: %s\n", strerror(errno));
		return -ENOENT;
	}

	/* iterates through all planes of a certain device */
	for (i = 0; (i < plane_res->count_planes) && !found_primary; i++) {
		int plane_id = plane_res->planes[i];

		drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
		if (!plane) {
			log_err("drmModeGetPlane(%u) failed: %s\n", plane_id, strerror(errno));
			continue;
		}

		/* check if the plane can be used by our CRTC */
		if (plane->possible_crtcs & (1 << ddrm->crtc_index)) {
			drmModeObjectPropertiesPtr props =
				drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);

			if (get_property_value(fd, props, "type") == DRM_PLANE_TYPE_PRIMARY) {
				found_primary = true;
				ddrm->plane.id = plane_id;
				ret = 0;
			}
			drmModeFreeObjectProperties(props);
		}
		drmModeFreePlane(plane);
	}
	drmModeFreePlaneResources(plane_res);

	if (found_primary)
		log_debug("found primary plane, id: %d\n", ddrm->plane.id);
	else
		log_warn("couldn't find a primary plane\n");
	return ret;
}

/*
 * When switching from a GUI to kmscon VT, the mouse cursor can stay in the middle of the screen
 * so force disable all cursor planes.
 */
static void modeset_clear_cursor(drmModeAtomicReq *req, int fd)
{
	drmModePlaneResPtr plane_res;
	int i;

	plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res)
		return;

	/* iterates through all planes of a certain device */
	for (i = 0; (i < plane_res->count_planes); i++) {
		int plane_id = plane_res->planes[i];

		drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
		if (!plane) {
			log_err("drmModeGetPlane(%u) failed: %s\n", plane_id, strerror(errno));
			continue;
		}
		drmModeObjectPropertiesPtr props =
			drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
		if (get_property_value(fd, props, "type") == DRM_PLANE_TYPE_CURSOR) {
			uint32_t prop_id = get_property_id(fd, props, "CRTC_ID");

			if (drmModeAtomicAddProperty(req, plane_id, prop_id, 0) < 0)
				log_warn("Unable to set CRTC_ID to disable cursor");

			prop_id = get_property_id(fd, props, "FB_ID");
			if (drmModeAtomicAddProperty(req, plane_id, prop_id, 0) < 0)
				log_warn("Unable to set FB_ID to disable cursor");
		}

		drmModeFreeObjectProperties(props);
		drmModeFreePlane(plane);
	}
	drmModeFreePlaneResources(plane_res);
}

static void modeset_drm_object_fini(struct drm_object *obj)
{
	if (!obj->props)
		return;
	for (int i = 0; i < obj->props->count_props; i++)
		drmModeFreeProperty(obj->props_info[i]);
	free(obj->props_info);
	drmModeFreeObjectProperties(obj->props);
	obj->props = NULL;
}

static int modeset_setup_objects(int fd, struct uterm_drm_display *ddrm)
{
	struct drm_object *connector = &ddrm->connector;
	struct drm_object *crtc = &ddrm->crtc;
	struct drm_object *plane = &ddrm->plane;

	/* retrieve connector properties from the device */
	modeset_get_object_properties(fd, connector, DRM_MODE_OBJECT_CONNECTOR);
	if (!connector->props)
		goto out_conn;

	/* retrieve CRTC properties from the device */
	modeset_get_object_properties(fd, crtc, DRM_MODE_OBJECT_CRTC);
	if (!crtc->props)
		goto out_crtc;

	/* retrieve plane properties from the device */
	modeset_get_object_properties(fd, plane, DRM_MODE_OBJECT_PLANE);
	if (!plane->props)
		goto out_plane;

	return 0;

out_plane:
	modeset_drm_object_fini(crtc);
out_crtc:
	modeset_drm_object_fini(connector);
out_conn:
	return -ENOMEM;
}

void uterm_drm_display_free_properties(struct uterm_display *disp)
{
	struct uterm_drm_display *ddrm = disp->data;
	struct uterm_drm_video *vdrm = disp->video->data;

	modeset_drm_object_fini(&ddrm->connector);
	modeset_drm_object_fini(&ddrm->crtc);
	modeset_drm_object_fini(&ddrm->plane);

	drmModeDestroyPropertyBlob(vdrm->fd, ddrm->mode_blob_id);
}

int uterm_drm_prepare_commit(int fd, struct uterm_drm_display *ddrm, drmModeAtomicReq *req,
			     uint32_t fb, uint32_t width, uint32_t height)
{
	struct drm_object *plane = &ddrm->plane;

	/* set id of the CRTC id that the connector is using */
	if (set_drm_object_property(req, &ddrm->connector, "CRTC_ID", ddrm->crtc.id) < 0)
		return -1;

	/* set the mode id of the CRTC; this property receives the id of a blob
	 * property that holds the struct that actually contains the mode info */
	if (set_drm_object_property(req, &ddrm->crtc, "MODE_ID", ddrm->mode_blob_id) < 0)
		return -1;

	/* set the CRTC object as active */
	if (set_drm_object_property(req, &ddrm->crtc, "ACTIVE", 1) < 0)
		return -1;

	/* set properties of the plane related to the CRTC and the framebuffer */
	if (set_drm_object_property(req, plane, "FB_ID", fb) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_ID", ddrm->crtc.id) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_X", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_Y", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_W", width << 16) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_H", height << 16) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_X", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_Y", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_W", width) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_H", height) < 0)
		return -1;

	return 0;
}

int uterm_drm_set_dpms(int fd, uint32_t conn_id, int state)
{
	int i, ret, set;
	drmModeConnector *conn;
	drmModePropertyRes *prop;

	switch (state) {
	case UTERM_DPMS_ON:
		set = DRM_MODE_DPMS_ON;
		break;
	case UTERM_DPMS_STANDBY:
		set = DRM_MODE_DPMS_STANDBY;
		break;
	case UTERM_DPMS_SUSPEND:
		set = DRM_MODE_DPMS_SUSPEND;
		break;
	case UTERM_DPMS_OFF:
		set = DRM_MODE_DPMS_OFF;
		break;
	default:
		return -EINVAL;
	}

	conn = drmModeGetConnector(fd, conn_id);
	if (!conn) {
		log_err("cannot get display connector");
		return -EFAULT;
	}

	ret = state;
	for (i = 0; i < conn->count_props; ++i) {
		prop = drmModeGetProperty(fd, conn->props[i]);
		if (!prop) {
			log_error("cannot get DRM property (%d): %m", errno);
			continue;
		}

		if (!strcmp(prop->name, "DPMS")) {
			ret = drmModeConnectorSetProperty(fd, conn_id, prop->prop_id, set);
			if (ret) {
				log_warn("cannot set DPMS %d", ret);
				ret = -EFAULT;
			}
			drmModeFreeProperty(prop);
			break;
		}
		drmModeFreeProperty(prop);
	}

	if (i == conn->count_props) {
		log_warn("display does not support DPMS");
		ret = UTERM_DPMS_UNKNOWN;
	}

	drmModeFreeConnector(conn);
	return ret;
}

int uterm_drm_get_dpms(int fd, drmModeConnector *conn)
{
	int i, ret;
	drmModePropertyRes *prop;

	for (i = 0; i < conn->count_props; ++i) {
		prop = drmModeGetProperty(fd, conn->props[i]);
		if (!prop) {
			log_error("cannot get DRM property (%d): %m", errno);
			continue;
		}

		if (!strcmp(prop->name, "DPMS")) {
			switch (conn->prop_values[i]) {
			case DRM_MODE_DPMS_ON:
				ret = UTERM_DPMS_ON;
				break;
			case DRM_MODE_DPMS_STANDBY:
				ret = UTERM_DPMS_STANDBY;
				break;
			case DRM_MODE_DPMS_SUSPEND:
				ret = UTERM_DPMS_SUSPEND;
				break;
			case DRM_MODE_DPMS_OFF:
			default:
				ret = UTERM_DPMS_OFF;
			}

			drmModeFreeProperty(prop);
			return ret;
		}
		drmModeFreeProperty(prop);
	}

	if (i == conn->count_props)
		log_warn("display does not support DPMS");
	return UTERM_DPMS_UNKNOWN;
}

int uterm_drm_display_set_dpms(struct uterm_display *disp, int state)
{
	int ret;
	struct uterm_drm_display *ddrm = disp->data;
	struct uterm_drm_video *vdrm = disp->video->data;

	log_info("setting DPMS of display %s to %s\n", disp->name, uterm_dpms_to_name(state));

	ret = uterm_drm_set_dpms(vdrm->fd, ddrm->connector.id, state);
	if (ret < 0)
		return ret;

	disp->dpms = ret;
	return 0;
}

int uterm_drm_display_wait_pflip(struct uterm_display *disp)
{
	struct uterm_video *video = disp->video;
	int ret;
	unsigned int timeout = 1000; /* 1s */

	if ((disp->flags & DISPLAY_PFLIP) || !(disp->flags & DISPLAY_VSYNC))
		return 0;

	do {
		ret = uterm_drm_video_wait_pflip(video, &timeout);
		if (ret < 1)
			break;
		else if ((disp->flags & DISPLAY_PFLIP))
			break;
	} while (timeout > 0);

	if (ret < 0)
		return ret;
	if (ret == 0 || !timeout) {
		log_warning("timeout waiting for page-flip on display %p", disp);
		return -ETIMEDOUT;
	}

	return 0;
}

static int perform_modeset(struct uterm_video *video)
{
	drmModeAtomicReq *req;
	struct shl_dlist *iter;
	struct uterm_drm_video *vdrm = video->data;
	struct uterm_display *disp;
	struct uterm_drm_display *ddrm;
	int flags;
	int ret = 0;

	/* prepare modeset on all outputs */
	req = drmModeAtomicAlloc();
	if (!req)
		return -ENOMEM;

	modeset_clear_cursor(req, vdrm->fd);

	shl_dlist_for_each(iter, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		ddrm = disp->data;

		uterm_drm_display_wait_pflip(disp);

		log_info("Preparing modeset for %s at %dx%d\n", disp->name,
			 ddrm->current_mode->hdisplay, ddrm->current_mode->vdisplay);

		ret = ddrm->prepare_modeset(disp, req);
		if (ret < 0)
			break;
	}
	if (ret < 0) {
		log_err("prepare atomic commit failed, %d\n", ret);
		return ret;
	}

	/* perform test-only atomic commit */
	flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(vdrm->fd, req, flags, NULL);
	if (ret < 0) {
		log_err("test-only atomic commit failed, %d\n", ret);
		ret = -EAGAIN;
		goto err_commit;
	}

	shl_dlist_for_each(iter, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		uterm_display_ref(disp);
	}

	/* initial modeset on all outputs */
	flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
	ret = drmModeAtomicCommit(vdrm->fd, req, flags, video);
	if (ret < 0)
		log_err("modeset atomic commit failed, %d\n", ret);

err_commit:
	drmModeAtomicFree(req);

	shl_dlist_for_each(iter, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		ddrm = disp->data;
		ddrm->done_modeset(disp, ret);
		if (ret) {
			disp->flags &= ~DISPLAY_ONLINE;
			uterm_display_unref(disp);
		} else
			disp->flags |= DISPLAY_ONLINE;
	}
	return ret;
}

static int try_modeset(struct uterm_video *video)
{
	struct shl_dlist *iter;
	struct uterm_display *disp;
	struct uterm_drm_display *ddrm;
	int ret;

	ret = perform_modeset(video);

	if (ret != -EAGAIN)
		return ret;

	/* Retry with default mode for all display */
	shl_dlist_for_each(iter, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		ddrm = disp->data;
		ddrm->current_mode = &ddrm->default_mode;
	}
	return perform_modeset(video);
}

static int pageflip(int fd, struct uterm_display *disp, uint32_t fb)
{
	struct uterm_drm_display *ddrm = disp->data;
	drmModeAtomicReq *req;
	int ret, flags;
	uint32_t width, height;

	/* prepare output for atomic commit */
	req = drmModeAtomicAlloc();

	height = disp->height;
	width = disp->width;

	ret = uterm_drm_prepare_commit(fd, ddrm, req, fb, width, height);
	if (ret) {
		log_warn("prepare atomic pageflip failed for [%s], %d\n", disp->name, ret);
		return -EINVAL;
	}

	flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
	ret = drmModeAtomicCommit(fd, req, flags, disp->video);
	drmModeAtomicFree(req);

	if (ret < 0) {
		/* don't print error for EBUSY, as next pageflip will succeed */
		if (ret != -EBUSY)
			log_warn("atomic pageflip failed for [%s], %d\n", disp->name, ret);
		return ret;
	}
	return 0;
}

int uterm_drm_display_swap(struct uterm_display *disp, uint32_t fb)
{
	struct uterm_video *video = disp->video;
	struct uterm_drm_video *vdrm = video->data;
	int ret;

	if (disp->dpms != UTERM_DPMS_ON)
		return -EINVAL;

	if ((disp->flags & DISPLAY_VSYNC))
		return -EBUSY;

	ret = pageflip(vdrm->fd, disp, fb);
	if (ret)
		return ret;

	/* take a ref on display, so that it won't get free before the pageflip
	 * callback occurs */
	uterm_display_ref(disp);
	disp->flags |= DISPLAY_VSYNC;

	return 0;
}

bool uterm_drm_is_swapping(struct uterm_display *disp)
{
	return (disp->flags & DISPLAY_VSYNC) != 0;
}

static void uterm_drm_display_pflip(struct uterm_display *disp)
{
	struct uterm_drm_video *vdrm = disp->video->data;

	disp->flags &= ~(DISPLAY_PFLIP | DISPLAY_VSYNC);
	if (vdrm->page_flip)
		vdrm->page_flip(disp);

	DISPLAY_CB(disp, UTERM_PAGE_FLIP);
}

static void display_event(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
			  unsigned int crtc_id, void *data)
{
	struct uterm_video *video = data;
	struct shl_dlist *iter;
	struct uterm_display *disp;
	struct uterm_drm_display *ddrm;

	shl_dlist_for_each(iter, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		ddrm = disp->data;
		if (ddrm->crtc.id == crtc_id) {
			if (disp->flags & DISPLAY_VSYNC)
				disp->flags |= DISPLAY_PFLIP;

			uterm_display_unref(disp);
			return;
		}
	}
	log_warning("Received display event for an unknown display crtc_id: %d", crtc_id);
}

static int uterm_drm_video_read_events(struct uterm_video *video)
{
	struct uterm_drm_video *vdrm = video->data;
	drmEventContext ev;
	int ret;

	/* TODO: DRM subsystem does not support non-blocking reads and it also
	 * doesn't return 0/-1 if the device is dead. This can lead to serious
	 * deadlocks in userspace if we read() after a device was unplugged. Fix
	 * this upstream and then make this code actually loop. */
	memset(&ev, 0, sizeof(ev));
	ev.version = DRM_EVENT_CONTEXT_VERSION;
	ev.page_flip_handler2 = display_event;
	errno = 0;
	ret = drmHandleEvent(vdrm->fd, &ev);

	if (ret < 0 && errno != EAGAIN)
		return -EFAULT;

	return 0;
}

static void do_pflips(struct ev_eloop *eloop, void *unused, void *data)
{
	struct uterm_video *video = data;
	struct uterm_display *disp;
	struct shl_dlist *iter;

	shl_dlist_for_each(iter, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		if ((disp->flags & DISPLAY_PFLIP))
			uterm_drm_display_pflip(disp);
	}
}

static void io_event(struct ev_fd *fd, int mask, void *data)
{
	struct uterm_video *video = data;
	struct uterm_drm_video *vdrm = video->data;
	struct uterm_display *disp;
	struct shl_dlist *iter;
	int ret;

	/* TODO: forward HUP to caller */
	if (mask & (EV_HUP | EV_ERR)) {
		log_err("error or hangup on DRM fd");
		ev_eloop_rm_fd(vdrm->efd);
		vdrm->efd = NULL;
		return;
	}

	if (!(mask & EV_READABLE))
		return;

	ret = uterm_drm_video_read_events(video);
	if (ret)
		return;

	shl_dlist_for_each(iter, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		if ((disp->flags & DISPLAY_PFLIP))
			uterm_drm_display_pflip(disp);
	}
}

static void vt_timeout(struct ev_timer *timer, uint64_t exp, void *data)
{
	struct uterm_video *video = data;
	struct uterm_drm_video *vdrm = video->data;
	struct uterm_display *disp;
	struct shl_dlist *iter;
	int r;

	r = uterm_drm_video_wake_up(video);
	if (!r) {
		ev_timer_update(vdrm->vt_timer, NULL);
		shl_dlist_for_each(iter, &video->displays)
		{
			disp = shl_dlist_entry(iter, struct uterm_display, list);
			VIDEO_CB(video, disp, UTERM_REFRESH);
		}
	}
}

void uterm_drm_video_arm_vt_timer(struct uterm_video *video)
{
	struct uterm_drm_video *vdrm = video->data;
	struct itimerspec spec;

	spec.it_value.tv_sec = 0;
	spec.it_value.tv_nsec = 20L * 1000L * 1000L; /* 20ms */
	spec.it_interval = spec.it_value;

	ev_timer_update(vdrm->vt_timer, &spec);
}

int uterm_drm_video_init(struct uterm_video *video, const char *node,
			 const struct display_ops *display_ops, uterm_drm_page_flip_t pflip,
			 void *data)
{
	struct uterm_drm_video *vdrm;
	int ret;

	log_info("new drm device via %s", node);

	vdrm = malloc(sizeof(*vdrm));
	if (!vdrm)
		return -ENOMEM;
	memset(vdrm, 0, sizeof(*vdrm));
	video->data = vdrm;
	vdrm->data = data;
	vdrm->page_flip = pflip;
	vdrm->display_ops = display_ops;

	vdrm->fd = open(node, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (vdrm->fd < 0) {
		log_err("cannot open drm device %s (%d): %m", node, errno);
		ret = -EFAULT;
		goto err_free;
	}
	/* TODO: fix the race-condition with DRM-Master-on-open */
	drmDropMaster(vdrm->fd);

	ret = drmSetClientCap(vdrm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		log_err("Device %s doesn't support universal planes", node);
		goto err_close;
	}

	ret = drmSetClientCap(vdrm->fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		log_err("Device %s doesn't support atomic modesetting", node);
		goto err_close;
	}

	ret = ev_eloop_new_fd(video->eloop, &vdrm->efd, vdrm->fd, EV_READABLE, io_event, video);
	if (ret)
		goto err_close;

	ret = shl_timer_new(&vdrm->timer);
	if (ret)
		goto err_fd;

	ret = ev_eloop_new_timer(video->eloop, &vdrm->vt_timer, NULL, vt_timeout, video);
	if (ret)
		goto err_timer;

	video->flags |= VIDEO_HOTPLUG;
	return 0;

err_timer:
	shl_timer_free(vdrm->timer);
err_fd:
	ev_eloop_rm_fd(vdrm->efd);
err_close:
	close(vdrm->fd);
err_free:
	free(vdrm);
	return ret;
}

void uterm_drm_video_destroy(struct uterm_video *video)
{
	struct uterm_drm_video *vdrm = video->data;

	ev_eloop_rm_timer(vdrm->vt_timer);
	ev_eloop_unregister_idle_cb(video->eloop, do_pflips, video, EV_SINGLE);
	shl_timer_free(vdrm->timer);
	ev_eloop_rm_fd(vdrm->efd);
	close(vdrm->fd);
	free(video->data);
}

static drmModeCrtc *get_current_crtc(int fd, uint32_t encoder_id)
{
	drmModeEncoder *enc = drmModeGetEncoder(fd, encoder_id);
	if (enc) {
		int crtc_id = enc->crtc_id;
		drmModeFreeEncoder(enc);
		return drmModeGetCrtc(fd, crtc_id);
	}
	return NULL;
}

static bool is_mode_null(drmModeModeInfoPtr mode)
{
	return mode->hdisplay == 0;
}

static void init_modes(struct uterm_display *disp, drmModeConnector *conn)
{
	struct uterm_video *video = disp->video;
	struct uterm_drm_video *vdrm = disp->video->data;
	struct uterm_drm_display *ddrm = disp->data;
	drmModeCrtc *current_crtc;
	drmModeModeInfoPtr mode;
	int i;

	current_crtc = get_current_crtc(vdrm->fd, conn->encoder_id);

	for (i = 0; i < conn->count_modes; ++i) {
		mode = &conn->modes[i];

		/* Use the mode marked as preferred, or the first if none is marked */
		if (is_mode_null(&ddrm->default_mode) || mode->type & DRM_MODE_TYPE_PREFERRED)
			ddrm->default_mode = *mode;

		/* Save the original KMS mode for later use */
		if (current_crtc &&
		    memcmp(&conn->modes[i], &current_crtc->mode, sizeof(conn->modes[i])) == 0)
			ddrm->original_mode = *mode;

		if (is_mode_null(&ddrm->desired_mode) && video->desired_width != 0 &&
		    video->desired_height != 0 && mode->hdisplay == video->desired_width &&
		    mode->vdisplay == video->desired_height)
			ddrm->desired_mode = *mode;
	}
	if (current_crtc)
		drmModeFreeCrtc(current_crtc);

	if (video->use_original)
		ddrm->current_mode = &ddrm->original_mode;
	else if (!is_mode_null(&ddrm->desired_mode))
		ddrm->current_mode = &ddrm->desired_mode;
	else
		ddrm->current_mode = &ddrm->default_mode;

	log_debug("Original mode %dx%d\n", ddrm->original_mode.hdisplay,
		  ddrm->original_mode.vdisplay);
	log_debug("Default mode %dx%d\n", ddrm->default_mode.hdisplay, ddrm->default_mode.vdisplay);
	log_debug("Desired mode %dx%d\n", ddrm->desired_mode.hdisplay, ddrm->desired_mode.vdisplay);
	log_debug("Trying mode %dx%d\n", ddrm->current_mode->hdisplay,
		  ddrm->current_mode->vdisplay);
}

static void bind_display(struct uterm_video *video, drmModeRes *res, drmModeConnector *conn)
{
	struct uterm_drm_video *vdrm = video->data;
	struct uterm_display *disp;
	struct uterm_drm_display *ddrm;
	const char *name;
	int ret;

	name = drmModeGetConnectorTypeName(conn->connector_type);

	ret = display_new(&disp, vdrm->display_ops, video, name);
	if (ret)
		return;
	ddrm = disp->data;
	init_modes(disp, conn);

	ddrm->connector.id = conn->connector_id;
	disp->dpms = UTERM_DPMS_ON;
	uterm_drm_display_set_dpms(disp, disp->dpms);

	log_info("display %s DPMS is %s", disp->name, uterm_dpms_to_name(disp->dpms));

	if (drmModeCreatePropertyBlob(vdrm->fd, ddrm->current_mode, sizeof(ddrm->mode),
				      &ddrm->mode_blob_id) != 0) {
		log_err("couldn't create a blob property\n");
		goto err_unref;
	}

	/* find a crtc for this connector */
	ret = modeset_find_crtc(video, vdrm->fd, res, conn, ddrm);
	if (ret) {
		log_err("no valid crtc for connector %u\n", conn->connector_id);
		goto out_blob;
	}

	/* with a connector and crtc, find a primary plane */
	ret = modeset_find_plane(vdrm->fd, ddrm);
	if (ret) {
		log_err("no valid plane for crtc %u\n", ddrm->crtc.id);
		goto out_blob;
	}

	/* gather properties of our connector, CRTC and planes */
	ret = modeset_setup_objects(vdrm->fd, ddrm);
	if (ret) {
		log_err("cannot get plane properties\n");
		goto out_blob;
	}
	disp->flags |= DISPLAY_AVAILABLE;
	uterm_display_bind(disp);

	uterm_display_unref(disp);
	return;

out_blob:
	drmModeDestroyPropertyBlob(vdrm->fd, ddrm->mode_blob_id);

err_unref:
	uterm_display_unref(disp);
	return;
}

int uterm_drm_video_hotplug(struct uterm_video *video, bool read_dpms, bool modeset)
{
	struct uterm_drm_video *vdrm = video->data;
	drmModeRes *res;
	drmModeConnector *conn;
	struct uterm_display *disp;
	struct uterm_drm_display *ddrm;
	int ret, i, dpms;
	struct shl_dlist *iter, *tmp;
	bool new_display = false;

	if (!video_is_awake(video) || !video_need_hotplug(video))
		return 0;

	log_debug("testing DRM hotplug status");

	res = drmModeGetResources(vdrm->fd);
	if (!res) {
		log_err("cannot retrieve drm resources");
		return -EACCES;
	}

	shl_dlist_for_each(iter, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		disp->flags &= ~DISPLAY_AVAILABLE;
	}

	for (i = 0; i < res->count_connectors; ++i) {
		conn = drmModeGetConnector(vdrm->fd, res->connectors[i]);
		if (!conn)
			continue;
		if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0) {
			drmModeFreeConnector(conn);
			continue;
		}

		shl_dlist_for_each(iter, &video->displays)
		{
			disp = shl_dlist_entry(iter, struct uterm_display, list);
			ddrm = disp->data;

			if (ddrm->connector.id != res->connectors[i])
				continue;

			disp->flags |= DISPLAY_AVAILABLE;

			if (!display_is_online(disp))
				break;

			if (read_dpms) {
				dpms = uterm_drm_get_dpms(vdrm->fd, conn);
				if (dpms != disp->dpms) {
					log_debug("DPMS state for display %p changed", disp);
					uterm_drm_display_set_dpms(disp, disp->dpms);
				}
			}
			break;
		}

		if (iter == &video->displays) {
			new_display = true;
			bind_display(video, res, conn);
		}
		drmModeFreeConnector(conn);
	}

	drmModeFreeResources(res);

	shl_dlist_for_each_safe(iter, tmp, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		if (!(disp->flags & DISPLAY_AVAILABLE))
			uterm_display_unbind(disp);
	}
	if (modeset || new_display) {
		ret = try_modeset(video);
		if (ret)
			return ret;
	}
	shl_dlist_for_each(iter, &video->displays)
	{
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		uterm_display_ready(disp);
	}

	video->flags &= ~VIDEO_HOTPLUG;
	return 0;
}

int uterm_drm_video_wake_up(struct uterm_video *video)
{
	int ret;
	struct uterm_drm_video *vdrm = video->data;

	ret = drmSetMaster(vdrm->fd);
	if (ret) {
		log_err("cannot set DRM-master");
		return -EACCES;
	}

	video->flags |= VIDEO_AWAKE | VIDEO_HOTPLUG;
	ret = uterm_drm_video_hotplug(video, true, true);
	if (ret) {
		drmDropMaster(vdrm->fd);
		return ret;
	}

	return 0;
}

void uterm_drm_video_sleep(struct uterm_video *video)
{
	struct uterm_drm_video *vdrm = video->data;

	drmDropMaster(vdrm->fd);
	ev_timer_drain(vdrm->vt_timer, NULL);
	ev_timer_update(vdrm->vt_timer, NULL);
}

int uterm_drm_video_poll(struct uterm_video *video)
{
	video->flags |= VIDEO_HOTPLUG;
	return uterm_drm_video_hotplug(video, false, false);
}

/* Waits for events on DRM fd for \mtimeout milliseconds and returns 0 if the
 * timeout expired, -ERR on errors and 1 if a page-flip event has been read.
 * \mtimeout is adjusted to the remaining time. */
int uterm_drm_video_wait_pflip(struct uterm_video *video, unsigned int *mtimeout)
{
	struct uterm_drm_video *vdrm = video->data;
	struct pollfd pfd;
	int ret;
	uint64_t elapsed;

	shl_timer_start(vdrm->timer);

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = vdrm->fd;
	pfd.events = POLLIN;

	log_debug("waiting for pageflip on %p", video);
	ret = poll(&pfd, 1, *mtimeout);

	elapsed = shl_timer_stop(vdrm->timer);
	*mtimeout = *mtimeout - (elapsed / 1000 + 1);

	if (ret < 0) {
		log_error("poll() failed on DRM fd (%d): %m", errno);
		return -EFAULT;
	} else if (!ret) {
		log_warning("timeout waiting for page-flip on %p", video);
		return 0;
	} else if ((pfd.revents & POLLIN)) {
		ret = uterm_drm_video_read_events(video);
		if (ret)
			return ret;

		ret = ev_eloop_register_idle_cb(video->eloop, do_pflips, video,
						EV_ONESHOT | EV_SINGLE);
		if (ret)
			return ret;

		return 1;
	} else {
		log_debug("poll() HUP/ERR on DRM fd (%d)", pfd.revents);
		return -EFAULT;
	}
}
