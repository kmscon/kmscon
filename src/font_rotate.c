/*
 * kmscon - rotate font
 *
 * Copyright (c) 2025 Jocelyn Falempe <jfalempe@redhat.com>
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

#include "font_rotate.h"
#include "shl_misc.h"

SHL_EXPORT
int kmscon_rotate_create_tables(struct shl_hashtable **normal, struct shl_hashtable **bold,
				shl_free_cb free_glyph)
{
	int ret;

	ret = shl_hashtable_new(normal, shl_direct_hash, shl_direct_equal, free_glyph);
	if (ret)
		return ret;

	ret = shl_hashtable_new(bold, shl_direct_hash, shl_direct_equal, free_glyph);
	if (ret)
		shl_hashtable_free(*normal);
	return ret;
}

SHL_EXPORT
void kmscon_rotate_free_tables(struct shl_hashtable *normal, struct shl_hashtable *bold)
{
	shl_hashtable_free(normal);
	shl_hashtable_free(bold);
}

SHL_EXPORT
int kmscon_rotate_glyph(struct uterm_video_buffer *vb, const struct kmscon_glyph *glyph,
			enum Orientation orientation, uint8_t align)
{
	int width;
	int height;
	int stride;
	int i, j;
	uint8_t *dst, *src;
	const struct uterm_video_buffer *buf = &glyph->buf;

	if (orientation == OR_NORMAL || orientation == OR_UPSIDE_DOWN) {
		width = buf->width;
		height = buf->height;
	} else {
		width = buf->height;
		height = buf->width;
	}

	stride = align * ((width + (align - 1)) / align);
	vb->data = malloc(stride * height);

	if (!vb->data)
		return -ENOMEM;

	src = buf->data;
	dst = vb->data;

	switch (orientation) {
	default:
	case OR_NORMAL:
		for (i = 0; i < buf->height; i++) {
			memcpy(dst, src, buf->width);
			dst += stride;
			src += buf->stride;
		}
		break;
	case OR_RIGHT:
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++) {
				dst[j * stride + (width - i - 1)] = src[j];
			}
			src += buf->stride;
		}
		break;
	case OR_UPSIDE_DOWN:
		src += (buf->height - 1) * buf->stride;
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++)
				dst[j] = src[buf->width - j - 1];
			dst += stride;
			src -= buf->stride;
		}
		break;
	case OR_LEFT:
		for (i = 0; i < buf->height; i++) {
			for (j = 0; j < buf->width; j++) {
				dst[(height - j - 1) * stride + i] = src[j];
			}
			src += buf->stride;
		}
	}
	vb->width = width;
	vb->height = height;
	vb->stride = stride;
	vb->format = buf->format;
	return 0;
}
