/*
 * kmscon - PC Screen font backend module
 *
 * Copyright (c) 2026-present awsq.code <awsq.code@gmail.com>
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
 *
 * NOTICE:
 * Code may include fragments by Jecelyn Falempe <jfalempe@redgat.com>
 * licensed under MIT License
 */

/*
 * PC Screen font backend module
 * This module registers the text-font psf backend with kmscon.
 */
#include <stdlib.h>
#include "font.h"
#include "shl/log.h"
#include "shl/module_interface.h"

#define LOG_SUBSYSTEM "mod_psf"

static int kmscon_psf_load(void)
{
	int ret;

	kmscon_font_psf_ops.owner = SHL_THIS_MODULE;
	ret = kmscon_font_register(&kmscon_font_psf_ops);
	if (ret) {
		log_error("cannot register psf font");
		return ret;
	}

	return 0;
}

static void kmscon_psf_unload(void)
{
	kmscon_font_unregister(kmscon_font_psf_ops.name);
}

SHL_MODULE(NULL, kmscon_psf_load, kmscon_psf_unload, NULL);
