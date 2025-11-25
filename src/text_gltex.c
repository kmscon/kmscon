/*
 * kmscon - OpenGL Textures Text Renderer Backend
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
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

/**
 * SECTION:text_gltex.c
 * @short_description: OpenGL Textures Text Renderer Backend
 * @include: text.h
 *
 * Uses OpenGL textures to store glyph information and draws these textures with
 * a custom fragment shader.
 * Glyphs are stored in texture-atlases. OpenGL has heavy restrictions on
 * texture sizes so we need to use multiple atlases. As there is no way to pass
 * a varying amount of textures to a shader, we need to render the screen for
 * each atlas we have.
 */

#define GL_GLEXT_PROTOTYPES

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "shl_dlist.h"
#include "shl_gl.h"
#include "shl_hashtable.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "text.h"
#include "text_gltex_atlas.frag.bin.h"
#include "text_gltex_atlas.vert.bin.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "text_gltex"

/* thanks khronos for breaking backwards compatibility.. */
#if !defined(GL_UNPACK_ROW_LENGTH) && defined(GL_UNPACK_ROW_LENGTH_EXT)
#define GL_UNPACK_ROW_LENGTH GL_UNPACK_ROW_LENGTH_EXT
#endif

struct atlas {
	struct shl_dlist list;

	GLuint tex;
	unsigned int height;
	unsigned int width;
	unsigned int count;
	unsigned int fill;

	unsigned int cache_size;
	unsigned int cache_num;
	GLfloat *cache_pos;
	GLfloat *cache_texpos;
	GLfloat *cache_fgcol;
	GLfloat *cache_bgcol;

	GLfloat advance_htex;
	GLfloat advance_vtex;
};

struct glyph {
	const struct kmscon_glyph *glyph;
	struct atlas *atlas;
	unsigned int texoff;
};

#define GLYPH_WIDTH(gly) ((gly)->glyph->buf.width)
#define GLYPH_HEIGHT(gly) ((gly)->glyph->buf.height)
#define GLYPH_STRIDE(gly) ((gly)->glyph->buf.stride)
#define GLYPH_DATA(gly) ((gly)->glyph->buf.data)

struct gltex {
	struct shl_hashtable *glyphs;
	struct shl_hashtable *bold_glyphs;
	unsigned int max_tex_size;
	bool supports_rowlen;
	bool previous_overflow;

	struct shl_dlist atlases;

	GLfloat advance_x;
	GLfloat advance_y;

	struct gl_shader *shader;
	GLuint uni_cos;
	GLuint uni_sin;
	GLuint uni_proj;
	GLuint uni_atlas;
	GLuint uni_advance_htex;
	GLuint uni_advance_vtex;

	unsigned int sw;
	unsigned int sh;

	GLfloat cos;
	GLfloat sin;
};

static int gltex_init(struct kmscon_text *txt)
{
	struct gltex *gt;

	gt = malloc(sizeof(*gt));
	if (!gt)
		return -ENOMEM;

	txt->data = gt;
	return 0;
}

static void gltex_destroy(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;

	free(gt);
}

static void free_glyph(void *data)
{
	struct glyph *glyph = data;

	free(glyph);
}

static int gltex_set(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	int ret, vlen, flen;
	const char *vert, *frag;
	static char *attr[] = {"position", "texture_position", "fgcolor", "bgcolor"};
	GLint s;
	const char *ext;

	if (!uterm_display_has_opengl(txt->disp))
		return -EINVAL;

	ret = uterm_display_use(txt->disp);
	if (ret < 0)
		return ret;

	memset(gt, 0, sizeof(*gt));
	shl_dlist_init(&gt->atlases);

	ret = shl_hashtable_new(&gt->glyphs, shl_direct_hash, shl_direct_equal, free_glyph);
	if (ret)
		return ret;

	ret = shl_hashtable_new(&gt->bold_glyphs, shl_direct_hash, shl_direct_equal, free_glyph);
	if (ret)
		goto err_htable;

	vert = _binary_text_gltex_atlas_vert_start;
	vlen = _binary_text_gltex_atlas_vert_size;
	frag = _binary_text_gltex_atlas_frag_start;
	flen = _binary_text_gltex_atlas_frag_size;
	gl_clear_error();

	ret = gl_shader_new(&gt->shader, vert, vlen, frag, flen, attr, 4, log_llog, NULL);
	if (ret)
		goto err_bold_htable;

	gt->uni_cos = gl_shader_get_uniform(gt->shader, "cos");
	gt->uni_sin = gl_shader_get_uniform(gt->shader, "sin");
	gt->uni_proj = gl_shader_get_uniform(gt->shader, "projection");
	gt->uni_atlas = gl_shader_get_uniform(gt->shader, "atlas");
	gt->uni_advance_htex = gl_shader_get_uniform(gt->shader, "advance_htex");
	gt->uni_advance_vtex = gl_shader_get_uniform(gt->shader, "advance_vtex");

	if (gl_has_error(gt->shader)) {
		log_warning("cannot create shader");
		goto err_shader;
	}

	gt->sw = uterm_display_get_width(txt->disp);
	gt->sh = uterm_display_get_height(txt->disp);

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		txt->cols = gt->sw / FONT_WIDTH(txt);
		txt->rows = gt->sh / FONT_HEIGHT(txt);
	} else {
		txt->cols = gt->sh / FONT_WIDTH(txt);
		txt->rows = gt->sw / FONT_HEIGHT(txt);
	}

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s);
	if (s <= 0)
		s = 64;
	else if (s > 2048)
		s = 2048;
	gt->max_tex_size = s;

	gl_clear_error();

	ext = (const char *)glGetString(GL_EXTENSIONS);
	if (ext && strstr((const char *)ext, "GL_EXT_unpack_subimage")) {
		gt->supports_rowlen = true;
	} else {
		log_warning("your GL implementation does not support GL_EXT_unpack_subimage, "
			    "glyph-rendering may be slower than usual");
	}

	return 0;

err_shader:
	gl_shader_unref(gt->shader);
err_bold_htable:
	shl_hashtable_free(gt->bold_glyphs);
err_htable:
	shl_hashtable_free(gt->glyphs);
	return ret;
}

static void gltex_unset(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	int ret;
	struct shl_dlist *iter;
	struct atlas *atlas;
	bool gl = true;

	ret = uterm_display_use(txt->disp);
	if (ret) {
		gl = false;
		log_warning("cannot activate OpenGL-CTX during destruction");
	}

	shl_hashtable_free(gt->bold_glyphs);
	shl_hashtable_free(gt->glyphs);

	while (!shl_dlist_empty(&gt->atlases)) {
		iter = gt->atlases.next;
		shl_dlist_unlink(iter);
		atlas = shl_dlist_entry(iter, struct atlas, list);

		free(atlas->cache_pos);
		free(atlas->cache_texpos);
		free(atlas->cache_fgcol);
		free(atlas->cache_bgcol);

		if (gl)
			gl_tex_free(&atlas->tex, 1);
		free(atlas);
	}

	if (gl) {
		gl_shader_unref(gt->shader);

		gl_clear_error();
	}
}

/* returns an atlas with at least 1 free glyph position; NULL on error */
static struct atlas *get_atlas(struct kmscon_text *txt, unsigned int num)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	size_t newsize;
	unsigned int width, height, nsize;
	GLenum err;

	/* check whether the last added atlas has still room for one glyph */
	if (!shl_dlist_empty(&gt->atlases)) {
		atlas = shl_dlist_entry(gt->atlases.next, struct atlas, list);
		if (atlas->fill + num <= atlas->count)
			return atlas;
	}

	/* all atlases are full so we have to create a new atlas */
	atlas = malloc(sizeof(*atlas));
	if (!atlas)
		return NULL;
	memset(atlas, 0, sizeof(*atlas));

	gl_clear_error();

	gl_tex_new(&atlas->tex, 1);
	err = glGetError();
	if (err != GL_NO_ERROR || !atlas->tex) {
		gl_clear_error();
		log_warning("cannot create new OpenGL texture: %d", err);
		goto err_free;
	}

	newsize = gt->max_tex_size / FONT_WIDTH(txt);
	if (newsize < 1)
		newsize = 1;

	/* OpenGL texture sizes are heavily restricted so we need to find a
	 * valid texture size that is big enough to hold as many glyphs as
	 * possible but at least 1 */
try_next:
	width = shl_next_pow2(FONT_WIDTH(txt) * newsize);
	height = shl_next_pow2(FONT_HEIGHT(txt));

	gl_clear_error();

	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
		     NULL);

	err = glGetError();
	if (err != GL_NO_ERROR) {
		if (newsize > 1) {
			--newsize;
			goto try_next;
		}
		gl_clear_error();
		log_warning("OpenGL textures too small for a single glyph (%d)", err);
		goto err_tex;
	}

	log_debug("new atlas of size %ux%u for %zu", width, height, newsize);

	nsize = txt->cols * txt->rows + 1; // +1 for the mouse pointer

	atlas->cache_pos = malloc(sizeof(GLfloat) * nsize * 2 * 6);
	if (!atlas->cache_pos)
		goto err_mem;

	atlas->cache_texpos = malloc(sizeof(GLfloat) * nsize * 2 * 6);
	if (!atlas->cache_texpos)
		goto err_mem;

	atlas->cache_fgcol = malloc(sizeof(GLfloat) * nsize * 3 * 6);
	if (!atlas->cache_fgcol)
		goto err_mem;

	atlas->cache_bgcol = malloc(sizeof(GLfloat) * nsize * 3 * 6);
	if (!atlas->cache_bgcol)
		goto err_mem;

	atlas->cache_size = nsize;
	atlas->count = newsize;
	atlas->width = width;
	atlas->height = height;
	atlas->advance_htex = 1.0 / atlas->width * FONT_WIDTH(txt);
	atlas->advance_vtex = 1.0 / atlas->height * FONT_HEIGHT(txt);

	shl_dlist_link(&gt->atlases, &atlas->list);
	return atlas;

err_mem:
	free(atlas->cache_pos);
	free(atlas->cache_texpos);
	free(atlas->cache_fgcol);
	free(atlas->cache_bgcol);
err_tex:
	gl_tex_free(&atlas->tex, 1);
err_free:
	free(atlas);
	return NULL;
}

static int find_glyph(struct kmscon_text *txt, struct glyph **out, uint64_t id, const uint32_t *ch,
		      size_t len, const struct tsm_screen_attr *attr)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct glyph *glyph;
	bool res;
	int ret, i;
	GLenum err;
	uint8_t *packed_data, *dst, *src;
	struct shl_hashtable *gtable;
	struct kmscon_font *font;

	if (attr->bold) {
		gtable = gt->bold_glyphs;
		font = txt->bold_font;
	} else {
		gtable = gt->glyphs;
		font = txt->font;
	}

	if (attr->underline)
		font->attr.underline = true;
	else
		font->attr.underline = false;

	if (attr->italic)
		font->attr.italic = true;
	else
		font->attr.italic = false;

	res = shl_hashtable_find(gtable, (void **)&glyph, id);
	if (res) {
		*out = glyph;
		return 0;
	}

	glyph = malloc(sizeof(*glyph));
	if (!glyph)
		return -ENOMEM;
	memset(glyph, 0, sizeof(*glyph));

	if (!len)
		ret = kmscon_font_render_empty(font, &glyph->glyph);
	else
		ret = kmscon_font_render(font, id, ch, len, &glyph->glyph);

	if (ret) {
		ret = kmscon_font_render_inval(font, &glyph->glyph);
		if (ret)
			goto err_free;
	}

	atlas = get_atlas(txt, glyph->glyph->width);
	if (!atlas) {
		ret = -EFAULT;
		goto err_free;
	}

	/* Funnily, not all OpenGLESv2 implementations support specifying the
	 * stride of a texture. Therefore, we then need to create a
	 * temporary image with a stride equal to the image width for loading
	 * the texture. This may slow down loading new glyphs but doesn't affect
	 * overall rendering performance. But driver developers should really
	 * add this! */

	gl_clear_error();

	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	if (!gt->supports_rowlen) {
		if (GLYPH_STRIDE(glyph) == GLYPH_WIDTH(glyph)) {
			glTexSubImage2D(GL_TEXTURE_2D, 0, FONT_WIDTH(txt) * atlas->fill, 0,
					GLYPH_WIDTH(glyph), GLYPH_HEIGHT(glyph), GL_ALPHA,
					GL_UNSIGNED_BYTE, GLYPH_DATA(glyph));
		} else {
			packed_data = malloc(GLYPH_WIDTH(glyph) * GLYPH_HEIGHT(glyph));
			if (!packed_data) {
				log_error("cannot allocate memory for glyph storage");
				ret = -ENOMEM;
				goto err_free;
			}

			src = GLYPH_DATA(glyph);
			dst = packed_data;
			for (i = 0; i < GLYPH_HEIGHT(glyph); ++i) {
				memcpy(dst, src, GLYPH_WIDTH(glyph));
				dst += GLYPH_WIDTH(glyph);
				src += GLYPH_STRIDE(glyph);
			}

			glTexSubImage2D(GL_TEXTURE_2D, 0, FONT_WIDTH(txt) * atlas->fill, 0,
					GLYPH_WIDTH(glyph), GLYPH_HEIGHT(glyph), GL_ALPHA,
					GL_UNSIGNED_BYTE, packed_data);
			free(packed_data);
		}
	} else {
		glPixelStorei(GL_UNPACK_ROW_LENGTH, GLYPH_STRIDE(glyph));
		glTexSubImage2D(GL_TEXTURE_2D, 0, FONT_WIDTH(txt) * atlas->fill, 0,
				GLYPH_WIDTH(glyph), GLYPH_HEIGHT(glyph), GL_ALPHA, GL_UNSIGNED_BYTE,
				GLYPH_DATA(glyph));
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

	/* Check for GL-errors
	 * As OpenGL is a state-machine, we cannot really tell which call failed
	 * without adding a glGetError() after each call. This is totally
	 * overkill so let us at least catch the error afterwards.
	 * We also add a hint to disable OpenGL if this does not work. This
	 * should _always_ work but OpenGL is kind of a black-box that isn't
	 * verbose at all and many things can go wrong. */

	err = glGetError();
	if (err != GL_NO_ERROR) {
		gl_clear_error();
		log_warning("cannot load glyph data into OpenGL texture (%d: %s); disable the "
			    "GL-renderer if this does not work reliably",
			    err, gl_err_to_str(err));
		ret = -EFAULT;
		goto err_free;
	}

	glyph->atlas = atlas;
	glyph->texoff = atlas->fill;

	ret = shl_hashtable_insert(gtable, id, glyph);
	if (ret)
		goto err_free;

	atlas->fill += glyph->glyph->width;

	*out = glyph;
	return 0;

err_free:
	free(glyph);
	return ret;
}

static void gltex_set_rotate(struct gltex *gt, enum Orientation orientation)
{
	float sin_table[5] = {0.0, 1.0, 0.0, -1.0, 0.0};

	gt->cos = sin_table[orientation + 1];
	gt->sin = sin_table[orientation];
}

static int gltex_rotate(struct kmscon_text *txt, enum Orientation orientation)
{
	struct gltex *gt = txt->data;

	txt->orientation = orientation;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		txt->cols = gt->sw / FONT_WIDTH(txt);
		txt->rows = gt->sh / FONT_HEIGHT(txt);
		gt->advance_x = 2.0 / gt->sw * FONT_WIDTH(txt);
		gt->advance_y = 2.0 / gt->sh * FONT_HEIGHT(txt);
	} else {
		float aspect = (float)gt->sw / (float)gt->sh;
		txt->cols = gt->sh / FONT_WIDTH(txt);
		txt->rows = gt->sw / FONT_HEIGHT(txt);
		gt->advance_x = 2.0 / gt->sw * FONT_WIDTH(txt) * aspect;
		gt->advance_y = 2.0 / gt->sh * FONT_HEIGHT(txt) * (1. / aspect);
	}
	gltex_unset(txt);
	gltex_set(txt);
	return 0;
}

static int gltex_prepare(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct shl_dlist *iter;
	int ret;

	ret = uterm_display_use(txt->disp);
	if (ret)
		return ret;

	shl_dlist_for_each(iter, &gt->atlases)
	{
		atlas = shl_dlist_entry(iter, struct atlas, list);

		atlas->cache_num = 0;
	}

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		gt->advance_x = 2.0 / gt->sw * FONT_WIDTH(txt);
		gt->advance_y = 2.0 / gt->sh * FONT_HEIGHT(txt);
	} else {
		float aspect = (float)gt->sw / (float)gt->sh;
		gt->advance_x = 2.0 / gt->sw * FONT_WIDTH(txt) * aspect;
		gt->advance_y = 2.0 / gt->sh * FONT_HEIGHT(txt) * (1. / aspect);
	}
	gltex_set_rotate(gt, txt->orientation);

	return 0;
}

static int gltex_draw(struct kmscon_text *txt, uint64_t id, const uint32_t *ch, size_t len,
		      unsigned int width, unsigned int posx, unsigned int posy,
		      const struct tsm_screen_attr *attr)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct glyph *glyph;
	float gl_x1, gl_x2, gl_y1, gl_y2;
	int ret, i, idx;

	if (!width)
		return 0;

	if (!len && posx && gt->previous_overflow) {
		gt->previous_overflow = false;
		return 0;
	}
	ret = find_glyph(txt, &glyph, id, ch, len, attr);
	if (ret)
		return ret;
	atlas = glyph->atlas;

	if (width == 1 && glyph->glyph->width == 2) {
		gt->previous_overflow = true;
		width = 2;
	} else {
		gt->previous_overflow = false;
	}

	if (atlas->cache_num >= atlas->cache_size)
		return -ERANGE;

	idx = atlas->cache_num * 2 * 6;
	gl_x1 = gt->advance_x * posx - 1.0;
	gl_x2 = gl_x1 + width * gt->advance_x;
	gl_y1 = 1.0 - gt->advance_y * posy;
	gl_y2 = gl_y1 - gt->advance_y;

	atlas->cache_pos[idx + 0] = gl_x1;
	atlas->cache_pos[idx + 1] = gl_y1;
	atlas->cache_pos[idx + 2] = gl_x1;
	atlas->cache_pos[idx + 3] = gl_y2;
	atlas->cache_pos[idx + 4] = gl_x2;
	atlas->cache_pos[idx + 5] = gl_y2;

	atlas->cache_pos[idx + 6] = gl_x1;
	atlas->cache_pos[idx + 7] = gl_y1;
	atlas->cache_pos[idx + 8] = gl_x2;
	atlas->cache_pos[idx + 9] = gl_y2;
	atlas->cache_pos[idx + 10] = gl_x2;
	atlas->cache_pos[idx + 11] = gl_y1;

	atlas->cache_texpos[idx + 0] = glyph->texoff;
	atlas->cache_texpos[idx + 1] = 0.0;
	atlas->cache_texpos[idx + 2] = glyph->texoff;
	atlas->cache_texpos[idx + 3] = 1.0;
	atlas->cache_texpos[idx + 4] = glyph->texoff + width;
	atlas->cache_texpos[idx + 5] = 1.0;

	atlas->cache_texpos[idx + 6] = glyph->texoff;
	atlas->cache_texpos[idx + 7] = 0.0;
	atlas->cache_texpos[idx + 8] = glyph->texoff + width;
	atlas->cache_texpos[idx + 9] = 1.0;
	atlas->cache_texpos[idx + 10] = glyph->texoff + width;
	atlas->cache_texpos[idx + 11] = 0.0;

	for (i = 0; i < 6; ++i) {
		idx = atlas->cache_num * 3 * 6 + i * 3;
		if (attr->inverse) {
			atlas->cache_fgcol[idx + 0] = attr->br / 255.0;
			atlas->cache_fgcol[idx + 1] = attr->bg / 255.0;
			atlas->cache_fgcol[idx + 2] = attr->bb / 255.0;
			atlas->cache_bgcol[idx + 0] = attr->fr / 255.0;
			atlas->cache_bgcol[idx + 1] = attr->fg / 255.0;
			atlas->cache_bgcol[idx + 2] = attr->fb / 255.0;
		} else {
			atlas->cache_fgcol[idx + 0] = attr->fr / 255.0;
			atlas->cache_fgcol[idx + 1] = attr->fg / 255.0;
			atlas->cache_fgcol[idx + 2] = attr->fb / 255.0;
			atlas->cache_bgcol[idx + 0] = attr->br / 255.0;
			atlas->cache_bgcol[idx + 1] = attr->bg / 255.0;
			atlas->cache_bgcol[idx + 2] = attr->bb / 255.0;
		}
	}

	++atlas->cache_num;

	return 0;
}

static int gltex_draw_pointer(struct kmscon_text *txt, unsigned int x, unsigned int y,
			      const struct tsm_screen_attr *attr)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct glyph *glyph;
	float gl_x1, gl_x2, gl_y1, gl_y2;
	unsigned int sw, sh;
	int ret, i, idx;
	uint32_t ch = 'I';
	uint64_t id = ch;

	ret = find_glyph(txt, &glyph, id, &ch, 1, attr);
	if (ret)
		return ret;

	atlas = glyph->atlas;

	if (atlas->cache_num >= atlas->cache_size)
		return -ERANGE;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		sw = gt->sw;
		sh = gt->sh;
	} else {
		sw = gt->sh;
		sh = gt->sw;
	}

	if (x > sw)
		x = sw;

	if (y > sh)
		y = sh;

	gl_x1 = x * 2.0 / sw - 1.0 - gt->advance_x / 2.0;
	gl_y1 = 1.0 - y * 2.0 / sh + gt->advance_y / 2.0;
	gl_x2 = gl_x1 + gt->advance_x;
	gl_y2 = gl_y1 - gt->advance_y;

	idx = atlas->cache_num * 2 * 6;

	atlas->cache_pos[idx + 0] = gl_x1;
	atlas->cache_pos[idx + 1] = gl_y1;
	atlas->cache_pos[idx + 2] = gl_x1;
	atlas->cache_pos[idx + 3] = gl_y2;
	atlas->cache_pos[idx + 4] = gl_x2;
	atlas->cache_pos[idx + 5] = gl_y2;

	atlas->cache_pos[idx + 6] = gl_x1;
	atlas->cache_pos[idx + 7] = gl_y1;
	atlas->cache_pos[idx + 8] = gl_x2;
	atlas->cache_pos[idx + 9] = gl_y2;
	atlas->cache_pos[idx + 10] = gl_x2;
	atlas->cache_pos[idx + 11] = gl_y1;

	atlas->cache_texpos[idx + 0] = glyph->texoff;
	atlas->cache_texpos[idx + 1] = 0.0;
	atlas->cache_texpos[idx + 2] = glyph->texoff;
	atlas->cache_texpos[idx + 3] = 1.0;
	atlas->cache_texpos[idx + 4] = glyph->texoff + 1.0;
	atlas->cache_texpos[idx + 5] = 1.0;

	atlas->cache_texpos[idx + 6] = glyph->texoff;
	atlas->cache_texpos[idx + 7] = 0.0;
	atlas->cache_texpos[idx + 8] = glyph->texoff + 1.0;
	atlas->cache_texpos[idx + 9] = 1.0;
	atlas->cache_texpos[idx + 10] = glyph->texoff + 1.0;
	atlas->cache_texpos[idx + 11] = 0.0;

	for (i = 0; i < 6; ++i) {
		idx = atlas->cache_num * 3 * 6 + i * 3;
		atlas->cache_fgcol[idx + 0] = attr->fr / 255.0;
		atlas->cache_fgcol[idx + 1] = attr->fg / 255.0;
		atlas->cache_fgcol[idx + 2] = attr->fb / 255.0;
		atlas->cache_bgcol[idx + 0] = attr->br / 255.0;
		atlas->cache_bgcol[idx + 1] = attr->bg / 255.0;
		atlas->cache_bgcol[idx + 2] = attr->bb / 255.0;
	}

	++atlas->cache_num;

	return 0;
}

static int gltex_render(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct shl_dlist *iter;
	float mat[16];

	gl_clear_error();

	gl_shader_use(gt->shader);

	glViewport(0, 0, gt->sw, gt->sh);
	glDisable(GL_BLEND);

	gl_m4_identity(mat);
	glUniformMatrix4fv(gt->uni_proj, 1, GL_FALSE, mat);
	glUniform1f(gt->uni_cos, gt->cos);
	glUniform1f(gt->uni_sin, gt->sin);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glEnableVertexAttribArray(3);

	glActiveTexture(GL_TEXTURE0);
	glUniform1i(gt->uni_atlas, 0);

	shl_dlist_for_each(iter, &gt->atlases)
	{
		atlas = shl_dlist_entry(iter, struct atlas, list);
		if (!atlas->cache_num)
			continue;

		glBindTexture(GL_TEXTURE_2D, atlas->tex);
		glUniform1f(gt->uni_advance_htex, atlas->advance_htex);
		glUniform1f(gt->uni_advance_vtex, atlas->advance_vtex);

		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, atlas->cache_pos);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, atlas->cache_texpos);
		glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, atlas->cache_fgcol);
		glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 0, atlas->cache_bgcol);
		glDrawArrays(GL_TRIANGLES, 0, 6 * atlas->cache_num);
	}

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	glDisableVertexAttribArray(3);

	if (gl_has_error(gt->shader)) {
		log_warning("rendering console caused OpenGL errors");
		return -EFAULT;
	}

	return 0;
}

struct kmscon_text_ops kmscon_text_gltex_ops = {
	.name = "gltex",
	.owner = NULL,
	.init = gltex_init,
	.destroy = gltex_destroy,
	.set = gltex_set,
	.unset = gltex_unset,
	.rotate = gltex_rotate,
	.prepare = gltex_prepare,
	.draw = gltex_draw,
	.draw_pointer = gltex_draw_pointer,
	.render = gltex_render,
	.abort = NULL,
};
