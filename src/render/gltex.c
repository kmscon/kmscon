/*
 * kmscon - OpenGL Textures Render Backend
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
 * SECTION:render_gltex.c
 * @short_description: OpenGL Textures Render Backend
 * @include: render_internal.h
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
#include "font/font.h"
#include "gltex_atlas.frag.bin.h"
#include "gltex_atlas.vert.bin.h"
#include "render_internal.h"
#include "shl/dlist.h"
#include "shl/gl.h"
#include "shl/hashtable.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "render_gltex"

struct atlas {
	struct dlist list;

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

struct gl_glyph {
	bool double_width;
	struct atlas *atlas;
	unsigned int texoff;
};

struct gltex {
	struct shl_hashtable *glyphs;
	unsigned int max_tex_size;
	bool previous_overflow;

	struct dlist atlases;

	/* console origin and projection, in pixels of the rotated console */
	unsigned int off_x;
	unsigned int off_y;
	GLfloat proj[16];

	struct gl_shader *shader;
	GLuint uni_proj;
	GLuint uni_atlas;
	GLuint uni_advance_htex;
	GLuint uni_advance_vtex;

	unsigned int sw;
	unsigned int sh;

	struct tsm_attr attr;
};

static int gltex_init(struct render *rdr)
{
	struct gltex *gt;

	gt = calloc(1, sizeof(*gt));
	if (!gt)
		return -ENOMEM;

	rdr->data = gt;
	return 0;
}

static void gltex_destroy(struct render *rdr)
{
	struct gltex *gt = rdr->data;

	free(gt);
}

static void free_glyph(void *data)
{
	struct gl_glyph *glyph = data;

	free(glyph);
}

/* screen size in the orientation of the console */
static void screen_size(struct render *rdr, unsigned int *w, unsigned int *h)
{
	struct gltex *gt = rdr->data;

	if (rdr->orientation == OR_NORMAL || rdr->orientation == OR_UPSIDE_DOWN) {
		*w = gt->sw;
		*h = gt->sh;
	} else {
		*w = gt->sh;
		*h = gt->sw;
	}
}

/*
 * Projection from console pixels (origin at the top-left corner of the rotated
 * console, y going down) to OpenGL coordinates, rotated by the orientation.
 */
static void compute_projection(struct render *rdr)
{
	struct gltex *gt = rdr->data;
	static const float sin_table[5] = {0.0, 1.0, 0.0, -1.0, 0.0};
	float c = sin_table[rdr->orientation + 1];
	float s = sin_table[rdr->orientation];
	unsigned int cw, ch;
	float *m = gt->proj;

	screen_size(rdr, &cw, &ch);

	gl_m4_identity(m);
	m[0] = 2.0 * c / cw;
	m[1] = -2.0 * s / cw;
	m[4] = -2.0 * s / ch;
	m[5] = -2.0 * c / ch;
	m[12] = s - c;
	m[13] = s + c;
}

static void compute_offset(struct render *rdr)
{
	struct gltex *gt = rdr->data;
	unsigned int cw, ch;

	screen_size(rdr, &cw, &ch);
	gt->off_x = (cw - rdr->cols * FONT_WIDTH(rdr)) / 2;
	gt->off_y = (ch - rdr->rows * FONT_HEIGHT(rdr)) / 2;

	/* the cursor offset is in screen pixels, not rotated */
	if (rdr->orientation == OR_NORMAL || rdr->orientation == OR_UPSIDE_DOWN)
		display_set_cursor_offset(rdr->disp, gt->off_x, gt->off_y);
	else
		display_set_cursor_offset(rdr->disp, gt->off_y, gt->off_x);
}

static int gltex_set(struct render *rdr)
{
	struct gltex *gt = rdr->data;
	int ret, vlen, flen;
	const char *vert, *frag;
	static char *attr[] = {"position", "texture_position", "fgcolor", "bgcolor"};
	GLint s;

	if (!display_has_opengl(rdr->disp))
		return -EINVAL;

	ret = display_use(rdr->disp);
	if (ret < 0)
		return ret;

	memset(gt, 0, sizeof(*gt));
	dlist_init(&gt->atlases);

	ret = shl_hashtable_new(&gt->glyphs, shl_direct_hash, shl_direct_equal, free_glyph);
	if (ret)
		return ret;

	vert = _binary_gltex_atlas_vert_start;
	vlen = _binary_gltex_atlas_vert_size;
	frag = _binary_gltex_atlas_frag_start;
	flen = _binary_gltex_atlas_frag_size;
	gl_clear_error();

	ret = gl_shader_new(&gt->shader, vert, vlen, frag, flen, attr,
			    sizeof(attr) / sizeof(*attr));
	if (ret)
		goto err_htable;

	gt->uni_proj = gl_shader_get_uniform(gt->shader, "projection");
	gt->uni_atlas = gl_shader_get_uniform(gt->shader, "atlas");
	gt->uni_advance_htex = gl_shader_get_uniform(gt->shader, "advance_htex");
	gt->uni_advance_vtex = gl_shader_get_uniform(gt->shader, "advance_vtex");

	if (gl_has_error(gt->shader)) {
		log_warning("cannot create shader");
		ret = -EFAULT;
		goto err_shader;
	}

	gt->sw = display_get_width(rdr->disp);
	gt->sh = display_get_height(rdr->disp);

	rdr->max_cols = render_get_cols(rdr, FONT_WIDTH(rdr));
	rdr->max_rows = render_get_rows(rdr, FONT_HEIGHT(rdr));
	rdr->cols = rdr->max_cols;
	rdr->rows = rdr->max_rows;
	compute_offset(rdr);
	compute_projection(rdr);

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s);
	if (s <= 0)
		s = 64;
	else if (s > 2048)
		s = 2048;
	gt->max_tex_size = s;

	gl_clear_error();
	return 0;

err_shader:
	gl_shader_unref(gt->shader);
err_htable:
	shl_hashtable_free(gt->glyphs);
	return ret;
}

/* frees an atlas, also a partially initialized one; the texture is only
 * released if @gl is true (the GL context is current) */
static void atlas_free(struct atlas *atlas, bool gl)
{
	free(atlas->cache_pos);
	free(atlas->cache_texpos);
	free(atlas->cache_fgcol);
	free(atlas->cache_bgcol);

	if (gl)
		gl_tex_free(&atlas->tex, 1);
	free(atlas);
}

static void gltex_unset(struct render *rdr)
{
	struct gltex *gt = rdr->data;
	int ret;
	struct dlist *tmp;
	struct atlas *atlas;
	bool gl = true;

	ret = display_use(rdr->disp);
	if (ret) {
		gl = false;
		log_warning("cannot activate OpenGL-CTX during destruction");
	}

	shl_hashtable_free(gt->glyphs);

	dlist_for_each_entry_safe(atlas, tmp, &gt->atlases, list)
	{
		dlist_unlink(&atlas->list);
		atlas_free(atlas, gl);
	}

	if (gl) {
		gl_shader_unref(gt->shader);

		gl_clear_error();
	}
}

/*
 * Allocates the texture storage of @atlas. OpenGL texture sizes are heavily
 * restricted so we need to find a valid texture size that is big enough to
 * hold as many glyphs as possible but at least 1. On success, sets the atlas
 * size and glyph count and returns 0.
 */
static int atlas_alloc_tex(struct render *rdr, struct atlas *atlas)
{
	struct gltex *gt = rdr->data;
	unsigned int width, height;
	size_t newsize;
	GLenum err;

	newsize = gt->max_tex_size / FONT_WIDTH(rdr);
	if (newsize < 1)
		newsize = 1;

	height = shl_next_pow2(FONT_HEIGHT(rdr));
	for (;; --newsize) {
		width = shl_next_pow2(FONT_WIDTH(rdr) * newsize);

		gl_clear_error();

		glBindTexture(GL_TEXTURE_2D, atlas->tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA,
			     GL_UNSIGNED_BYTE, NULL);

		err = glGetError();
		if (err == GL_NO_ERROR)
			break;
		if (newsize <= 1) {
			gl_clear_error();
			log_warning("OpenGL textures too small for a single glyph (%d)", err);
			return -EFAULT;
		}
	}

	log_debug("new atlas of size %ux%u for %zu", width, height, newsize);

	atlas->count = newsize;
	atlas->width = width;
	atlas->height = height;
	return 0;
}

/* returns an atlas with at least 1 free glyph position; NULL on error */
static struct atlas *get_atlas(struct render *rdr, unsigned int num)
{
	struct gltex *gt = rdr->data;
	struct atlas *atlas;
	unsigned int nsize;
	GLenum err;

	/* check whether the last added atlas has still room for one glyph */
	atlas = dlist_first(&gt->atlases, struct atlas, list);
	if (atlas && atlas->fill + num <= atlas->count)
		return atlas;

	/* all atlases are full so we have to create a new atlas */
	atlas = calloc(1, sizeof(*atlas));
	if (!atlas)
		return NULL;

	gl_clear_error();

	gl_tex_new(&atlas->tex, 1);
	err = glGetError();
	if (err != GL_NO_ERROR || !atlas->tex) {
		gl_clear_error();
		log_warning("cannot create new OpenGL texture: %d", err);
		goto err;
	}

	if (atlas_alloc_tex(rdr, atlas))
		goto err;

	nsize = rdr->max_cols * rdr->max_rows + 1; // +1 for the mouse pointer

	atlas->cache_pos = malloc(sizeof(GLfloat) * nsize * 2 * 6);
	atlas->cache_texpos = malloc(sizeof(GLfloat) * nsize * 2 * 6);
	atlas->cache_fgcol = malloc(sizeof(GLfloat) * nsize * 3 * 6);
	atlas->cache_bgcol = malloc(sizeof(GLfloat) * nsize * 3 * 6);
	if (!atlas->cache_pos || !atlas->cache_texpos || !atlas->cache_fgcol || !atlas->cache_bgcol)
		goto err;

	atlas->cache_size = nsize;
	atlas->advance_htex = 1.0 / atlas->width * FONT_WIDTH(rdr);
	atlas->advance_vtex = 1.0 / atlas->height * FONT_HEIGHT(rdr);

	dlist_link(&gt->atlases, &atlas->list);
	return atlas;

err:
	atlas_free(atlas, atlas->tex != 0);
	return NULL;
}

static struct gl_glyph *find_glyph(struct render *rdr, const struct tsm_cell *cell)
{
	struct gltex *gt = rdr->data;
	struct atlas *atlas;
	struct gl_glyph *glglyph;
	GLenum err;
	struct kmscon_font *font = rdr->font;
	unsigned int num;
	struct kmscon_glyph *glyph;
	struct kmscon_font_attr attr;
	uint32_t ch = cell->ch ? cell->ch : ' ';
	uint64_t id;

	attr.underline = !!(cell->attr & TSM_ATTR_UNDERLINE);
	attr.italic = !!(cell->attr & TSM_ATTR_ITALIC);
	attr.bold = !!(cell->attr & TSM_ATTR_BOLD);

	if ((cell->attr & TSM_ATTR_BLINK) && rdr->blinking)
		ch = ' ';

	if (!kmscon_font_has_glyph(font, &attr, ch))
		ch = FONT_REPLACEMENT_CHAR;

	id = kmscon_glyph_id(ch, cell->attr);

	if (shl_hashtable_find(gt->glyphs, (void **)&glglyph, id))
		return glglyph;

	glyph = kmscon_font_render(font, &attr, ch);
	if (!glyph)
		return NULL;

	glglyph = calloc(1, sizeof(*glglyph));
	if (!glglyph)
		goto err_free;

	glglyph->double_width = glyph->double_width;

	num = kmscon_glyph_cwidth(glyph);
	atlas = get_atlas(rdr, num);
	if (!atlas)
		goto err_free;

	gl_clear_error();

	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, FONT_WIDTH(rdr) * atlas->fill, 0, glyph->buf.width,
			glyph->buf.height, GL_ALPHA, GL_UNSIGNED_BYTE, glyph->buf.data);
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
		goto err_free;
	}

	glglyph->atlas = atlas;
	glglyph->texoff = atlas->fill;

	if (shl_hashtable_insert(gt->glyphs, id, glglyph))
		goto err_free;

	atlas->fill += num;
	free(glyph);

	return glglyph;

err_free:
	free(glyph);
	free(glglyph);
	return NULL;
}

static void gltex_resize(struct render *rdr, unsigned int cols, unsigned int rows)
{
	rdr->cols = cols;
	rdr->rows = rows;
	compute_offset(rdr);
}

static int gltex_rotate(struct render *rdr, enum Orientation orientation)
{
	rdr->orientation = orientation;

	gltex_unset(rdr);
	return gltex_set(rdr);
}

static int gltex_prepare(struct render *rdr, struct tsm_attr *attr)
{
	struct gltex *gt = rdr->data;
	struct atlas *atlas;
	int ret;

	ret = display_use(rdr->disp);
	if (ret)
		return ret;

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		atlas->cache_num = 0;
	}
	gt->attr = *attr;

	glClearColor(gt->attr.bg.r / 255.0, gt->attr.bg.g / 255.0, gt->attr.bg.b / 255.0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	return 0;
}

/*
 * Appends a glyph quad to the vertex caches of @atlas, as two triangles.
 * (x1, y1) is the top-left and (x2, y2) the bottom-right corner in GL
 * coordinates, the texture spans @width glyphs from @texoff.
 */
static int atlas_push_quad(struct atlas *atlas, float x1, float y1, float x2, float y2,
			   unsigned int texoff, float width, const struct tsm_color *fg,
			   const struct tsm_color *bg)
{
	GLfloat *pos, *texpos, *fgcol, *bgcol;
	int i;

	if (atlas->cache_num >= atlas->cache_size)
		return -ERANGE;

	pos = &atlas->cache_pos[atlas->cache_num * 2 * 6];
	pos[0] = x1;
	pos[1] = y1;
	pos[2] = x1;
	pos[3] = y2;
	pos[4] = x2;
	pos[5] = y2;

	pos[6] = x1;
	pos[7] = y1;
	pos[8] = x2;
	pos[9] = y2;
	pos[10] = x2;
	pos[11] = y1;

	texpos = &atlas->cache_texpos[atlas->cache_num * 2 * 6];
	texpos[0] = texoff;
	texpos[1] = 0.0;
	texpos[2] = texoff;
	texpos[3] = 1.0;
	texpos[4] = texoff + width;
	texpos[5] = 1.0;

	texpos[6] = texoff;
	texpos[7] = 0.0;
	texpos[8] = texoff + width;
	texpos[9] = 1.0;
	texpos[10] = texoff + width;
	texpos[11] = 0.0;

	fgcol = &atlas->cache_fgcol[atlas->cache_num * 3 * 6];
	bgcol = &atlas->cache_bgcol[atlas->cache_num * 3 * 6];
	for (i = 0; i < 6; ++i) {
		fgcol[i * 3 + 0] = fg->r / 255.0;
		fgcol[i * 3 + 1] = fg->g / 255.0;
		fgcol[i * 3 + 2] = fg->b / 255.0;
		bgcol[i * 3 + 0] = bg->r / 255.0;
		bgcol[i * 3 + 1] = bg->g / 255.0;
		bgcol[i * 3 + 2] = bg->b / 255.0;
	}

	++atlas->cache_num;
	return 0;
}

static int gltex_draw_cell(struct render *rdr, const struct tsm_cell *cell, unsigned int posx,
			   unsigned int posy)
{
	struct gltex *gt = rdr->data;
	struct gl_glyph *glglyph;
	float x1, x2, y1, y2;
	float width;
	int ret;

	if (posx && gt->previous_overflow && (cell->ch == 0 || cell->ch == ' ')) {
		gt->previous_overflow = false;
		return 0;
	}
	glglyph = find_glyph(rdr, cell);
	if (!glglyph)
		return -ENOMEM;

	width = glglyph->double_width ? 2.0 : 1.0;
	x1 = gt->off_x + posx * FONT_WIDTH(rdr);
	x2 = x1 + width * FONT_WIDTH(rdr);
	y1 = gt->off_y + posy * FONT_HEIGHT(rdr);
	y2 = y1 + FONT_HEIGHT(rdr);

	ret = atlas_push_quad(glglyph->atlas, x1, y1, x2, y2, glglyph->texoff, width, &cell->fg,
			      &cell->bg);
	if (ret)
		return ret;

	gt->previous_overflow = glglyph->double_width;
	return 0;
}

static int gltex_draw(struct render *rdr, const struct tsm_cell *cells,
		      struct render_cursor *cursor)
{
	unsigned int posx, posy, off;

	for (posy = 0; posy < rdr->rows; posy++) {
		for (posx = 0; posx < rdr->cols; posx++) {
			off = posx + posy * rdr->cols;

			if (cursor->visible && cursor->x == posx && cursor->y == posy)
				gltex_draw_cell(rdr, &cursor->cell, posx, posy);
			else if ((cells[off].attr & TSM_ATTR_BLINK) && rdr->blinking) {
				struct tsm_cell blank = cells[off];

				blank.ch = ' ';
				gltex_draw_cell(rdr, &blank, posx, posy);
			} else
				gltex_draw_cell(rdr, &cells[off], posx, posy);
		}
	}
	return 0;
}

static int gltex_draw_pointer(struct render *rdr, unsigned int x, unsigned int y)
{
	struct gltex *gt = rdr->data;
	struct gl_glyph *glyph;
	float x1, x2, y1, y2;
	unsigned int sw, sh;
	struct tsm_cell pointer_cell = {0};
	pointer_cell.ch = 'I';

	glyph = find_glyph(rdr, &pointer_cell);
	if (!glyph)
		return -ENOMEM;

	screen_size(rdr, &sw, &sh);

	if (x > sw)
		x = sw;

	if (y > sh)
		y = sh;

	/* center the pointer on (x, y), aligned on whole pixels */
	x1 = (int)(gt->off_x + x) - (int)(FONT_WIDTH(rdr) / 2);
	y1 = (int)(gt->off_y + y) - (int)(FONT_HEIGHT(rdr) / 2);
	x2 = x1 + FONT_WIDTH(rdr);
	y2 = y1 + FONT_HEIGHT(rdr);

	return atlas_push_quad(glyph->atlas, x1, y1, x2, y2, glyph->texoff, 1.0, &gt->attr.fg,
			       &gt->attr.bg);
}

static int gltex_render(struct render *rdr)
{
	struct gltex *gt = rdr->data;
	struct atlas *atlas;

	gl_clear_error();

	gl_shader_use(gt->shader);

	glViewport(0, 0, gt->sw, gt->sh);
	glDisable(GL_BLEND);

	glUniformMatrix4fv(gt->uni_proj, 1, GL_FALSE, gt->proj);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glEnableVertexAttribArray(3);

	glActiveTexture(GL_TEXTURE0);
	glUniform1i(gt->uni_atlas, 0);

	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
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

struct render_ops gltex_ops = {
	.name = "gltex",
	.owner = NULL,
	.init = gltex_init,
	.destroy = gltex_destroy,
	.set = gltex_set,
	.unset = gltex_unset,
	.resize = gltex_resize,
	.rotate = gltex_rotate,
	.prepare = gltex_prepare,
	.draw = gltex_draw,
	.draw_pointer = gltex_draw_pointer,
	.render = gltex_render,
};
