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

	/* the atlas is a grid of cols x rows glyph cells, filled row by row;
	 * fill is the index of the next free cell */
	unsigned int cols;
	unsigned int rows;
	unsigned int fill;
};

struct gl_glyph {
	struct atlas *atlas;
	/* width in console cells, 1 or 2 */
	unsigned int cwidth;
	/* top-left and bottom-right texture coordinates in the atlas */
	GLfloat u1, v1, u2, v2;
};

/* a glyph to draw, with its top-left corner at (x, y) in console pixels */
struct quad {
	const struct gl_glyph *glyph;
	int x, y;
	struct tsm_color fg;
	struct tsm_color bg;
};

struct vertex {
	GLfloat x, y; /* console pixels */
	GLfloat u, v; /* texture coordinates */
	/* RGBA, the alpha byte is unused but keeps the vertex 4-byte aligned */
	GLubyte fg[4];
	GLubyte bg[4];
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

	unsigned int sw;
	unsigned int sh;

	struct tsm_attr attr;

	/* the glyphs of the frame, in draw order, at most one per console cell,
	 * and the mouse pointer, drawn on top of them */
	struct quad *quads;
	unsigned int quads_size;
	unsigned int quads_num;
	struct quad pointer;
	bool pointer_visible;

	/* vertices of the quads drawn with one atlas, 6 per quad */
	struct vertex *vertices;
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

static void free_quads(struct gltex *gt)
{
	free(gt->quads);
	free(gt->vertices);
}

/* allocates room for one quad per console cell */
static int alloc_quads(struct render *rdr)
{
	struct gltex *gt = rdr->data;
	size_t n = (size_t)rdr->max_cols * rdr->max_rows;
	/* at least one quad, for the pointer */
	size_t nv = n ? n : 1;

	gt->quads = calloc(nv, sizeof(*gt->quads));
	gt->vertices = calloc(nv * 6, sizeof(*gt->vertices));
	if (!gt->quads || !gt->vertices) {
		free_quads(gt);
		return -ENOMEM;
	}

	gt->quads_size = n;
	return 0;
}

static int gltex_set(struct render *rdr)
{
	struct gltex *gt = rdr->data;
	int ret;
	static char *attr[] = {"position", "texture_position", "fgcolor", "bgcolor"};
	GLint s;

	if (!display_has_opengl(rdr->disp))
		return -EINVAL;

	ret = display_use(rdr->disp);
	if (ret < 0)
		return ret;

	memset(gt, 0, sizeof(*gt));
	dlist_init(&gt->atlases);

	ret = shl_hashtable_new(&gt->glyphs, shl_direct_hash, shl_direct_equal, free);
	if (ret)
		return ret;

	gl_clear_error();

	ret = gl_shader_new(&gt->shader, _binary_gltex_atlas_vert_start,
			    _binary_gltex_atlas_vert_size, _binary_gltex_atlas_frag_start,
			    _binary_gltex_atlas_frag_size, attr, sizeof(attr) / sizeof(*attr));
	if (ret)
		goto err_htable;

	gt->uni_proj = gl_shader_get_uniform(gt->shader, "projection");
	gt->uni_atlas = gl_shader_get_uniform(gt->shader, "atlas");

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

	ret = alloc_quads(rdr);
	if (ret)
		goto err_shader;

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s);
	if (s <= 0)
		s = 64;
	else if (s > 1024)
		s = 1024;
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
	free_quads(gt);

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
 * Allocates the texture storage of @atlas, as big as possible, up to
 * max_tex_size in both directions. OpenGL may refuse big textures, so halve
 * the height then the width until it succeeds, keeping room for at least one
 * glyph. On success, sets the atlas size and grid and returns 0.
 */
static int atlas_alloc_tex(struct render *rdr, struct atlas *atlas)
{
	struct gltex *gt = rdr->data;
	unsigned int width, height, min_width, min_height;
	GLenum err;

	/* largest power of 2 not above max_tex_size */
	width = shl_next_pow2(gt->max_tex_size);
	if (width > gt->max_tex_size)
		width /= 2;
	height = width;

	min_width = shl_next_pow2(FONT_WIDTH(rdr));
	min_height = shl_next_pow2(FONT_HEIGHT(rdr));
	width = width < min_width ? min_width : width;
	height = height < min_height ? min_height : height;

	for (;;) {
		gl_clear_error();

		glBindTexture(GL_TEXTURE_2D, atlas->tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0, GL_ALPHA,
			     GL_UNSIGNED_BYTE, NULL);

		err = glGetError();
		if (err == GL_NO_ERROR)
			break;

		if (height > min_height) {
			height /= 2;
		} else if (width > min_width) {
			width /= 2;
		} else {
			gl_clear_error();
			log_warning("OpenGL textures too small for a single glyph (%d)", err);
			return -EFAULT;
		}
	}

	atlas->width = width;
	atlas->height = height;
	atlas->cols = width / FONT_WIDTH(rdr);
	atlas->rows = height / FONT_HEIGHT(rdr);

	log_debug("new atlas of size %ux%u for %ux%u glyphs", width, height, atlas->cols,
		  atlas->rows);
	return 0;
}

/*
 * Returns the cell where a glyph @num cells wide would be stored in @atlas, as
 * an index in cells. Glyphs don't span two rows, so a glyph that doesn't fit
 * at the end of a row goes to the next one. The glyph fits if the returned
 * index + @num <= cols * rows.
 */
static unsigned int atlas_next_cell(const struct atlas *atlas, unsigned int num)
{
	unsigned int cell = atlas->fill;

	if (cell % atlas->cols + num > atlas->cols)
		cell += atlas->cols - cell % atlas->cols;
	return cell;
}

static bool atlas_has_room(const struct atlas *atlas, unsigned int num)
{
	return num <= atlas->cols && atlas_next_cell(atlas, num) + num <= atlas->cols * atlas->rows;
}

/* returns an atlas with room for a glyph @num cells wide; NULL on error */
static struct atlas *get_atlas(struct render *rdr, unsigned int num)
{
	struct gltex *gt = rdr->data;
	struct atlas *atlas;
	GLenum err;

	/* check whether the last added atlas has still room for the glyph */
	atlas = dlist_first(&gt->atlases, struct atlas, list);
	if (atlas && atlas_has_room(atlas, num))
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

	/* glyphs are drawn at their exact size, sample texels without blending */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	if (atlas_alloc_tex(rdr, atlas))
		goto err;

	if (!atlas_has_room(atlas, num)) {
		log_warning("OpenGL textures too small for a glyph of %u cells", num);
		goto err;
	}

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
	unsigned int slot, x, y;
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

	glglyph->cwidth = kmscon_glyph_cwidth(glyph);
	atlas = get_atlas(rdr, glglyph->cwidth);
	if (!atlas)
		goto err_free;

	/* position of the glyph in the atlas, in texels */
	slot = atlas_next_cell(atlas, glglyph->cwidth);
	x = slot % atlas->cols * FONT_WIDTH(rdr);
	y = slot / atlas->cols * FONT_HEIGHT(rdr);

	gl_clear_error();

	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, glyph->buf.width, glyph->buf.height, GL_ALPHA,
			GL_UNSIGNED_BYTE, glyph->buf.data);
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

	/* exact, as the atlas sizes are powers of 2 */
	glglyph->atlas = atlas;
	glglyph->u1 = (GLfloat)x / atlas->width;
	glglyph->v1 = (GLfloat)y / atlas->height;
	glglyph->u2 = (GLfloat)(x + glglyph->cwidth * FONT_WIDTH(rdr)) / atlas->width;
	glglyph->v2 = (GLfloat)(y + FONT_HEIGHT(rdr)) / atlas->height;

	if (shl_hashtable_insert(gt->glyphs, id, glglyph))
		goto err_free;

	atlas->fill = slot + glglyph->cwidth;
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
	int ret;

	ret = display_use(rdr->disp);
	if (ret)
		return ret;

	gt->quads_num = 0;
	gt->pointer_visible = false;
	gt->attr = *attr;

	glClearColor(gt->attr.bg.r / 255.0, gt->attr.bg.g / 255.0, gt->attr.bg.b / 255.0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	return 0;
}

static void set_vertex(struct vertex *v, float x, float y, float u, float t, const struct quad *q)
{
	*v = (struct vertex){
		.x = x,
		.y = y,
		.u = u,
		.v = t,
		.fg = {q->fg.r, q->fg.g, q->fg.b, 255},
		.bg = {q->bg.r, q->bg.g, q->bg.b, 255},
	};
}

/* writes the 6 vertices of quad @q, as two triangles, at index @idx */
static void emit_quad(struct render *rdr, unsigned int idx, const struct quad *q)
{
	struct gltex *gt = rdr->data;
	struct vertex *v = &gt->vertices[idx * 6];
	const struct gl_glyph *g = q->glyph;
	float x1 = q->x, y1 = q->y;
	float x2 = x1 + g->cwidth * FONT_WIDTH(rdr), y2 = y1 + FONT_HEIGHT(rdr);

	set_vertex(&v[0], x1, y1, g->u1, g->v1, q);
	set_vertex(&v[1], x1, y2, g->u1, g->v2, q);
	set_vertex(&v[2], x2, y2, g->u2, g->v2, q);

	set_vertex(&v[3], x1, y1, g->u1, g->v1, q);
	set_vertex(&v[4], x2, y2, g->u2, g->v2, q);
	set_vertex(&v[5], x2, y1, g->u2, g->v1, q);
}

static int gltex_draw_cell(struct render *rdr, const struct tsm_cell *cell, unsigned int posx,
			   unsigned int posy)
{
	struct gltex *gt = rdr->data;
	struct gl_glyph *glglyph;

	if (posx && gt->previous_overflow && (cell->ch == 0 || cell->ch == ' ')) {
		gt->previous_overflow = false;
		return 0;
	}
	glglyph = find_glyph(rdr, cell);
	if (!glglyph)
		return -ENOMEM;

	if (gt->quads_num >= gt->quads_size)
		return -ERANGE;

	gt->quads[gt->quads_num++] = (struct quad){
		.glyph = glglyph,
		.x = gt->off_x + posx * FONT_WIDTH(rdr),
		.y = gt->off_y + posy * FONT_HEIGHT(rdr),
		.fg = cell->fg,
		.bg = cell->bg,
	};

	gt->previous_overflow = glglyph->cwidth == 2;
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
	gt->pointer = (struct quad){
		.glyph = glyph,
		.x = (int)(gt->off_x + x) - (int)(FONT_WIDTH(rdr) / 2),
		.y = (int)(gt->off_y + y) - (int)(FONT_HEIGHT(rdr) / 2),
		.fg = gt->attr.fg,
		.bg = gt->attr.bg,
	};
	gt->pointer_visible = true;
	return 0;
}

/* draws the first @num quads of the vertex array, with the texture of @atlas */
static void draw_atlas(const struct atlas *atlas, unsigned int num)
{
	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glDrawArrays(GL_TRIANGLES, 0, 6 * num);
}

static int gltex_render(struct render *rdr)
{
	struct gltex *gt = rdr->data;
	const struct vertex *v = gt->vertices;
	const GLsizei stride = sizeof(*v);
	struct atlas *atlas;
	unsigned int i, num;

	gl_clear_error();

	gl_shader_use(gt->shader);

	glViewport(0, 0, gt->sw, gt->sh);
	glDisable(GL_BLEND);

	glUniformMatrix4fv(gt->uni_proj, 1, GL_FALSE, gt->proj);

	for (i = 0; i < 4; ++i)
		glEnableVertexAttribArray(i);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, &v->x);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, &v->u);
	/* normalized: the shader gets the colors in [0, 1] */
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, v->fg);
	glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, v->bg);

	glActiveTexture(GL_TEXTURE0);
	glUniform1i(gt->uni_atlas, 0);

	/* one draw call per atlas, reusing the vertex arrays, as client side
	 * arrays are consumed by glDrawArrays() */
	dlist_for_each_entry(atlas, &gt->atlases, list)
	{
		num = 0;
		for (i = 0; i < gt->quads_num; ++i)
			if (gt->quads[i].glyph->atlas == atlas)
				emit_quad(rdr, num++, &gt->quads[i]);
		if (num)
			draw_atlas(atlas, num);
	}

	if (gt->pointer_visible) {
		emit_quad(rdr, 0, &gt->pointer);
		draw_atlas(gt->pointer.glyph->atlas, 1);
	}

	for (i = 0; i < 4; ++i)
		glDisableVertexAttribArray(i);

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
