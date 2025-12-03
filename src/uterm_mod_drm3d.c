/*
 * kmscon - drm3d backend module
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
 * drm3d uterm video backend module
 * This module registers the drm3d backend with kmscon.
 */

#include <errno.h>
#include <stdlib.h>
#include "shl_log.h"
#include "shl_module_interface.h"
#include "uterm_video_internal.h"

extern struct uterm_video_module drm3d_module;

#define LOG_SUBSYSTEM "mod_drm3d"

static int kmscon_drm3d_load(void)
{
	int ret;

	drm3d_module.owner = SHL_THIS_MODULE;
	ret = uterm_video_register(&drm3d_module);
	if (ret) {
		log_error("cannot register drm3d font");
		return ret;
	}
	return 0;
}

static void kmscon_drm3d_unload(void)
{
	uterm_video_unregister(drm3d_module.name);
}

SHL_MODULE(NULL, kmscon_drm3d_load, kmscon_drm3d_unload, NULL);
