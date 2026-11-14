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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "font/font.h"
#include "gltex_atlas.frag.bin.h"
#include "gltex_atlas.vert.bin.h"
#include "shl/dlist.h"
#include "shl/gl.h"
#include "shl/hashtable.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "text.h"
#include "video/video.h"

#define LOG_SUBSYSTEM "text_gltex"

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

#define GLYPH_WIDTH(gly) ((gly)->buf.width)
#define GLYPH_HEIGHT(gly) ((gly)->buf.height)
#define GLYPH_STRIDE(gly) ((gly)->buf.stride)
#define GLYPH_DATA(gly) ((gly)->buf.data)

struct gltex_target {
	GLuint tex;
	GLuint fbo;
	unsigned int w;
	unsigned int h;
};

struct gltex {
	struct shl_hashtable *glyphs;
	unsigned int max_tex_size;
	bool previous_overflow;

	struct dlist atlases;

	GLfloat advance_x;
	GLfloat advance_y;
	GLfloat off_x;
	GLfloat off_y;

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

	struct tsm_screen_attr attr;

	/* post-processing fragment shader applied to the whole screen */
	struct gl_shader *post_shader;
	GLuint post_uni_source;
	GLuint post_uni_time;
	GLuint post_uni_resolution;
	struct timespec post_start;

	/* temporal ghosting/phosphor persistence: amount of the previous
	 * frame the post-processing shader keeps (0 = off) */
	GLfloat ghost_gamma;
	GLuint post_uni_history;
	GLuint post_uni_gamma;

	/* render targets used by the post-processing pipeline */
	struct gltex_target scene;
	/* single history buffer with the last presented frame; it is passed
	 * back into the post-processing shader as "history" */
	struct gltex_target hist;
	/* whether the scene texture holds the current console; render-only
	 * frames can skip the glyph pass and only re-run post-processing */
	bool scene_valid;

	/* live reload of the shader file */
	struct timespec shader_mtime;
	struct timespec shader_check_time;
	off_t shader_size;
	bool shader_missing;

	/* whether an unretryable post-processing setup failure was already
	 * reported; such failures are only logged once */
	bool post_setup_warned;
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
	struct gl_glyph *glyph = data;

	free(glyph);
}

static void gltex_set_cos(struct gltex *gt, enum Orientation orientation)
{
	float sin_table[5] = {0.0, 1.0, 0.0, -1.0, 0.0};

	gt->cos = sin_table[orientation + 1];
	gt->sin = sin_table[orientation];
}

static void compute_advance_and_offset(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	unsigned int off_x, off_y;

	if (txt->orientation == OR_NORMAL || txt->orientation == OR_UPSIDE_DOWN) {
		gt->advance_x = 2.0 / gt->sw * FONT_WIDTH(txt);
		gt->advance_y = 2.0 / gt->sh * FONT_HEIGHT(txt);
		off_x = (gt->sw - txt->cols * FONT_WIDTH(txt)) / 2;
		off_y = (gt->sh - txt->rows * FONT_HEIGHT(txt)) / 2;
		gt->off_x = (float)2.0 * off_x / gt->sw;
		gt->off_y = (float)2.0 * off_y / gt->sh;
	} else {
		gt->advance_x = 2.0 / gt->sh * FONT_WIDTH(txt);
		gt->advance_y = 2.0 / gt->sw * FONT_HEIGHT(txt);
		off_x = (gt->sw - txt->rows * FONT_HEIGHT(txt)) / 2;
		off_y = (gt->sh - txt->cols * FONT_WIDTH(txt)) / 2;
		gt->off_x = (float)2.0 * off_y / gt->sh;
		gt->off_y = (float)2.0 * off_x / gt->sw;
	}
	display_set_cursor_offset(txt->disp, off_x, off_y);
}

/* Post-processing pass
 * The whole console is first rendered into an off-screen framebuffer. That
 * framebuffer is then drawn to the screen again through a fragment shader:
 * the user-provided --render-shader file, or a built-in one when only
 * --render-ghosting is configured. The shader gets:
 *   varying vec2 texcoord;        texture coordinates of the screen
 *   uniform sampler2D source;     rendered console
 *   uniform float time;           seconds since the renderer was created
 *   uniform vec2 resolution;      screen size in pixels
 *   uniform sampler2D history;    last presented frame (ghosting only)
 *   uniform float gamma;          ghosting amount, 0..0.99 (ghosting only)
 * All uniforms are optional; missing ones are simply ignored.
 *
 * User shaders guard their history/gamma uniforms and blend code with
 * "#if HISTORY_ENABLED" around a "#define HISTORY_ENABLED 0" marker line;
 * while ghosting is configured that marker is flipped to 1 before compiling.
 * With ghosting enabled the shader blends source and history itself and the
 * result drawn to the screen is copied into the single history buffer
 * afterwards, so ghosting costs one extra pass and one history texture in
 * total.
 * Any failure disables post-processing and falls back to direct rendering. */

static const char post_vert_source[] = "attribute vec2 position;\n"
				       "varying vec2 texcoord;\n"
				       "void main()\n"
				       "{\n"
				       "	texcoord = position * 0.5 + 0.5;\n"
				       "	gl_Position = vec4(position, 0.0, 1.0);\n"
				       "}\n";

static const GLfloat post_vertices[] = {-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f,
					-1.0f, -1.0f, 1.0f,  1.0f, 1.0f, -1.0f};

/* built-in ghosting fragment shader: used as the post-processing shader when
 * ghosting is requested without a shader file:
 *   out = (1 - gamma) * current + gamma * history */
static const char ghost_frag_source[] =
	"precision mediump float;\n"
	"varying vec2 texcoord;\n"
	"uniform sampler2D source;\n"
	"uniform sampler2D history;\n"
	"uniform float gamma;\n"
	"void main()\n"
	"{\n"
	"	vec4 cur = texture2D(source, texcoord);\n"
	"	vec4 old = texture2D(history, texcoord);\n"
	"	gl_FragColor = vec4((1.0 - gamma) * cur.rgb + gamma * old.rgb, cur.a);\n"
	"}\n";

#define SHADER_FILE_MAX (4 * 1024 * 1024)
/* minimum time between two stat() calls on the shader file */
#define SHADER_CHECK_INTERVAL_NS (400 * 1000 * 1000)

static char *read_shader_file(const char *path, size_t *len_out)
{
	FILE *f;
	char *buf;
	long size;
	size_t readn;

	f = fopen(path, "re");
	if (!f) {
		log_warning("cannot open shader file '%s': %m", path);
		return NULL;
	}

	if (fseek(f, 0, SEEK_END)) {
		log_warning("cannot seek shader file '%s': %m", path);
		fclose(f);
		return NULL;
	}

	size = ftell(f);
	if (size < 0 || size > SHADER_FILE_MAX) {
		log_warning("invalid shader file size for '%s'", path);
		fclose(f);
		return NULL;
	}

	if (fseek(f, 0, SEEK_SET)) {
		log_warning("cannot rewind shader file '%s': %m", path);
		fclose(f);
		return NULL;
	}

	buf = malloc(size + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}

	readn = fread(buf, 1, size, f);
	if (readn != (size_t)size || ferror(f)) {
		log_warning("cannot read shader file '%s': %m", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[readn] = 0;
	fclose(f);

	if (!readn) {
		log_warning("shader file '%s' is empty", path);
		free(buf);
		return NULL;
	}

	*len_out = readn;
	return buf;
}

static void gltex_target_free(struct gltex_target *t)
{
	if (t->fbo) {
		glDeleteFramebuffers(1, &t->fbo);
		t->fbo = 0;
	}
	if (t->tex) {
		glDeleteTextures(1, &t->tex);
		t->tex = 0;
	}
	t->w = 0;
	t->h = 0;
}

static int gltex_target_create(struct gltex_target *t, unsigned int w, unsigned int h)
{
	GLenum status, err;

	gltex_target_free(t);
	gl_clear_error();

	glGenTextures(1, &t->tex);
	glBindTexture(GL_TEXTURE_2D, t->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glGenFramebuffers(1, &t->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	err = glGetError();
	if (err != GL_NO_ERROR || status != GL_FRAMEBUFFER_COMPLETE) {
		log_warning("cannot create render target %ux%u (%d: %s, status 0x%x)", w, h, err,
			    gl_err_to_str(err), status);
		gltex_target_free(t);
		gl_clear_error();
		return -EFAULT;
	}

	t->w = w;
	t->h = h;
	return 0;
}

static void gltex_target_clear(struct gltex_target *t)
{
	glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static int gltex_recreate_targets(struct gltex *gt)
{
	if (gltex_target_create(&gt->scene, gt->sw, gt->sh))
		return -EFAULT;
	gt->scene_valid = false;

	if (!gt->ghost_gamma)
		return 0;

	if (gltex_target_create(&gt->hist, gt->sw, gt->sh)) {
		gltex_target_free(&gt->hist);
		return -EFAULT;
	}
	gltex_target_clear(&gt->hist);
	return 0;
}

static void gltex_disable_post(struct gltex *gt)
{
	if (gt->post_shader)
		gl_shader_unref(gt->post_shader);
	gt->post_shader = NULL;

	gt->ghost_gamma = 0;
	gt->scene_valid = false;

	gltex_target_free(&gt->scene);
	gltex_target_free(&gt->hist);

	gl_clear_error();
}

static void gltex_record_shader_stat(struct gltex *gt, const char *path)
{
	struct stat st;

	if (stat(path, &st)) {
		gt->shader_missing = true;
		gt->shader_mtime.tv_sec = 0;
		gt->shader_mtime.tv_nsec = 0;
		gt->shader_size = 0;
		return;
	}

	gt->shader_missing = false;
	gt->shader_mtime = st.st_mtim;
	gt->shader_size = st.st_size;
}

static bool gltex_shader_check_due(struct gltex *gt)
{
	struct timespec now;
	long long diff;

	clock_gettime(CLOCK_MONOTONIC, &now);
	diff = (now.tv_sec - gt->shader_check_time.tv_sec) * 1000000000LL +
	       (now.tv_nsec - gt->shader_check_time.tv_nsec);
	if (diff < SHADER_CHECK_INTERVAL_NS)
		return false;

	gt->shader_check_time = now;
	return true;
}

static bool gltex_shader_changed(struct gltex *gt, const char *path)
{
	struct stat st;

	if (stat(path, &st))
		return !gt->shader_missing;

	if (gt->shader_missing)
		return true;

	return st.st_mtim.tv_sec != gt->shader_mtime.tv_sec ||
	       st.st_mtim.tv_nsec != gt->shader_mtime.tv_nsec || st.st_size != gt->shader_size;
}

/* Failures that cannot be retried without restarting kmscon are only
 * reported once so repeated kmscon_text_set() calls do not spam the log. */
static void gltex_post_setup_failed(struct kmscon_text *txt, const char *what)
{
	struct gltex *gt = txt->data;

	if (gt->post_setup_warned)
		return;
	gt->post_setup_warned = true;
	log_warning("%s failed; restart kmscon to try again", what);
}

/* User shaders carry a "#define HISTORY_ENABLED 0" line that guards their
 * history/gamma uniforms and blend code. When ghosting is configured, that
 * marker is flipped to 1 in-place (both are one character, so no reallocation
 * is needed) which enables the guarded code. */
static bool gltex_enable_ghosting(char *src)
{
	static const char off[] = "#define HISTORY_ENABLED 0";
	char *p = strstr(src, off);

	if (!p)
		return false;

	p[sizeof(off) - 2] = '1';
	return true;
}

static void gltex_setup_post(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	char *attrs[] = {"position"};
	char *src;
	size_t len;
	int ret;

	if ((!txt->render_shader || !txt->render_shader[0]) && !txt->render_ghosting)
		return;

	if (txt->render_shader && txt->render_shader[0]) {
		gltex_record_shader_stat(gt, txt->render_shader);

		src = read_shader_file(txt->render_shader, &len);
		if (!src)
			return;

		if (txt->render_ghosting && !gltex_enable_ghosting(src))
			log_warning("shader '%s' lacks a '#define HISTORY_ENABLED 0' line; "
				    "ghosting has no effect on it",
				    txt->render_shader);

		ret = gl_shader_new(&gt->post_shader, post_vert_source,
				    sizeof(post_vert_source) - 1, src, len, attrs, 1);
		free(src);
		if (ret) {
			log_warning("cannot compile shader '%s'; ignoring shader",
				    txt->render_shader);
			gt->post_shader = NULL;
			gl_clear_error();
			return;
		}
	} else {
		/* ghosting without a shader file: use the built-in
		 * (1 - gamma) * current + gamma * history shader */
		ret = gl_shader_new(&gt->post_shader, post_vert_source,
				    sizeof(post_vert_source) - 1, ghost_frag_source,
				    sizeof(ghost_frag_source) - 1, attrs, 1);
		if (ret) {
			log_warning("cannot create ghosting shader");
			gl_clear_error();
			gltex_post_setup_failed(txt, "creating the ghosting shader");
			return;
		}
	}

	if (gltex_target_create(&gt->scene, gt->sw, gt->sh)) {
		gl_shader_unref(gt->post_shader);
		gt->post_shader = NULL;
		gltex_post_setup_failed(txt, "creating the post-processing render target");
		return;
	}
	gt->scene_valid = false;

	gt->post_uni_source = gl_shader_get_uniform(gt->post_shader, "source");
	gt->post_uni_time = gl_shader_get_uniform(gt->post_shader, "time");
	gt->post_uni_resolution = gl_shader_get_uniform(gt->post_shader, "resolution");
	gt->post_uni_history = gl_shader_get_uniform(gt->post_shader, "history");
	gt->post_uni_gamma = gl_shader_get_uniform(gt->post_shader, "gamma");

	clock_gettime(CLOCK_MONOTONIC, &gt->post_start);

	if (txt->render_ghosting) {
		GLfloat gamma = txt->render_ghosting / 100.0;

		/* full persistence would never update the history and freeze
		 * the screen, so keep a bit of the current frame */
		if (gamma > 0.99)
			gamma = 0.99;

		if (gltex_target_create(&gt->hist, gt->sw, gt->sh)) {
			log_warning("cannot create ghosting history buffer; disabling ghosting");
			gltex_target_free(&gt->hist);
			gl_clear_error();
			gltex_post_setup_failed(txt, "creating the ghosting history buffer");
			return;
		}
		gltex_target_clear(&gt->hist);
		gt->ghost_gamma = gamma;
	}

	gt->post_setup_warned = false;
}

static GLfloat gltex_get_time(struct gltex *gt)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - gt->post_start.tv_sec) +
	       (now.tv_nsec - gt->post_start.tv_nsec) / 1000000000.0;
}

static void gltex_shader_reload(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;

	log_notice("shader file '%s' changed; reloading", txt->render_shader);
	gltex_disable_post(gt);
	gltex_setup_post(txt);

	if (gt->post_shader)
		log_notice("shader reloaded successfully");
	else
		log_warning("shader reload failed; rendering without shader");
}

static int gltex_set(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	int ret, vlen, flen;
	const char *vert, *frag;
	static char *attr[] = {"position", "texture_position", "fgcolor", "bgcolor"};
	GLint s;

	if (!display_has_opengl(txt->disp))
		return -EINVAL;

	ret = display_use(txt->disp);
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

	ret = gl_shader_new(&gt->shader, vert, vlen, frag, flen, attr, 4);
	if (ret)
		goto err_htable;

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

	gt->sw = display_get_width(txt->disp);
	gt->sh = display_get_height(txt->disp);

	txt->max_cols = kmscon_text_get_cols(txt, FONT_WIDTH(txt));
	txt->max_rows = kmscon_text_get_rows(txt, FONT_HEIGHT(txt));
	txt->cols = txt->max_cols;
	txt->rows = txt->max_rows;
	compute_advance_and_offset(txt);
	gltex_set_cos(gt, txt->orientation);

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s);
	if (s <= 0)
		s = 64;
	else if (s > 2048)
		s = 2048;
	gt->max_tex_size = s;

	gltex_setup_post(txt);

	gl_clear_error();
	return 0;

err_shader:
	gl_shader_unref(gt->shader);
err_htable:
	shl_hashtable_free(gt->glyphs);
	return ret;
}

static void gltex_unset(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	int ret;
	struct dlist *tmp;
	struct atlas *atlas;
	bool gl = true;

	ret = display_use(txt->disp);
	if (ret) {
		gl = false;
		log_warning("cannot activate OpenGL-CTX during destruction");
	}

	shl_hashtable_free(gt->glyphs);

	dlist_for_each_entry_safe(atlas, tmp, &gt->atlases, list)
	{
		dlist_unlink(&atlas->list);

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
		gltex_disable_post(gt);
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

	nsize = txt->max_cols * txt->max_rows + 1; // +1 for the mouse pointer

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

	dlist_link(&gt->atlases, &atlas->list);
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

static struct gl_glyph *find_glyph(struct kmscon_text *txt, const struct tsm_screen_cell *cell)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct gl_glyph *glglyph;
	GLenum err;
	struct kmscon_font *font = txt->font;
	unsigned int num;
	struct kmscon_glyph *glyph;
	struct kmscon_font_attr attr;
	uint32_t ch = cell->ch ? cell->ch : ' ';
	uint64_t id;

	attr.underline = !!cell->attr2.underline;
	attr.italic = !!cell->attr2.italic;
	attr.bold = !!cell->attr2.bold;

	if (cell->attr2.blink && txt->blinking)
		ch = ' ';

	if (!kmscon_font_has_glyph(font, &attr, ch))
		ch = FONT_REPLACEMENT_CHAR;

	id = kmscon_glyph_id(ch, cell->attr2.u8);

	if (shl_hashtable_find(gt->glyphs, (void **)&glglyph, id))
		return glglyph;

	glglyph = calloc(1, sizeof(*glglyph));
	if (!glglyph)
		return NULL;

	glyph = kmscon_font_render(font, &attr, ch);
	if (!glyph)
		return NULL;

	glglyph->double_width = glyph->double_width;

	num = kmscon_glyph_cwidth(glyph);
	atlas = get_atlas(txt, num);
	if (!atlas)
		goto err_free;

	gl_clear_error();

	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, FONT_WIDTH(txt) * atlas->fill, 0, GLYPH_WIDTH(glyph),
			GLYPH_HEIGHT(glyph), GL_ALPHA, GL_UNSIGNED_BYTE, GLYPH_DATA(glyph));
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

static void gltex_resize(struct kmscon_text *txt, unsigned int cols, unsigned int rows)
{
	txt->cols = cols;
	txt->rows = rows;
	compute_advance_and_offset(txt);
}

static int gltex_rotate(struct kmscon_text *txt, enum Orientation orientation)
{
	txt->orientation = orientation;

	gltex_unset(txt);
	gltex_set(txt);
	return 0;
}

static int gltex_prepare(struct kmscon_text *txt, struct tsm_screen_attr *attr)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	int ret;

	ret = display_use(txt->disp);
	if (ret)
		return ret;

	/* render-only rounds keep the cached glyph geometry of the last full
	 * rebuild, so only reset the caches when the content changed */
	if (txt->rebuild) {
		dlist_for_each_entry(atlas, &gt->atlases, list)
		{
			atlas->cache_num = 0;
		}
	}
	gt->attr = *attr;

	if (txt->render_shader && txt->render_shader[0] && gltex_shader_check_due(gt) &&
	    gltex_shader_changed(gt, txt->render_shader))
		gltex_shader_reload(txt);

	if (gt->post_shader && (gt->scene.w != gt->sw || gt->scene.h != gt->sh)) {
		if (gltex_recreate_targets(gt)) {
			log_warning("cannot resize render targets; ignoring shader");
			gltex_disable_post(gt);
		}
	}

	if (!gt->post_shader) {
		glClearColor(gt->attr.br / 255.0, gt->attr.bg / 255.0, gt->attr.bb / 255.0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
	} else if (txt->rebuild || !gt->scene_valid) {
		glBindFramebuffer(GL_FRAMEBUFFER, gt->scene.fbo);
		glClearColor(gt->attr.br / 255.0, gt->attr.bg / 255.0, gt->attr.bb / 255.0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	/* else: render-only frame; the off-screen scene texture still holds
	 * the current console, so keep it and only re-run post-processing */

	return 0;
}

static int gltex_draw_cell(struct kmscon_text *txt, const struct tsm_screen_cell *cell,
			   unsigned int posx, unsigned int posy)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct gl_glyph *glglyph;
	float gl_x1, gl_x2, gl_y1, gl_y2;
	float width;
	int i, idx;

	if (posx && gt->previous_overflow && (cell->ch == 0 || cell->ch == ' ')) {
		gt->previous_overflow = false;
		return 0;
	}
	glglyph = find_glyph(txt, cell);
	if (!glglyph)
		return -ENOMEM;

	atlas = glglyph->atlas;

	if (atlas->cache_num >= atlas->cache_size)
		return -ERANGE;

	width = glglyph->double_width ? 2.0 : 1.0;
	gt->previous_overflow = glglyph->double_width;

	idx = atlas->cache_num * 2 * 6;
	gl_x1 = gt->off_x + gt->advance_x * posx - 1.0;
	gl_x2 = gl_x1 + width * gt->advance_x;
	gl_y1 = 1.0 - gt->off_y - gt->advance_y * posy;
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

	atlas->cache_texpos[idx + 0] = glglyph->texoff;
	atlas->cache_texpos[idx + 1] = 0.0;
	atlas->cache_texpos[idx + 2] = glglyph->texoff;
	atlas->cache_texpos[idx + 3] = 1.0;
	atlas->cache_texpos[idx + 4] = glglyph->texoff + width;
	atlas->cache_texpos[idx + 5] = 1.0;

	atlas->cache_texpos[idx + 6] = glglyph->texoff;
	atlas->cache_texpos[idx + 7] = 0.0;
	atlas->cache_texpos[idx + 8] = glglyph->texoff + width;
	atlas->cache_texpos[idx + 9] = 1.0;
	atlas->cache_texpos[idx + 10] = glglyph->texoff + width;
	atlas->cache_texpos[idx + 11] = 0.0;

	for (i = 0; i < 6; ++i) {
		idx = atlas->cache_num * 3 * 6 + i * 3;
		atlas->cache_fgcol[idx + 0] = cell->fg.r / 255.0;
		atlas->cache_fgcol[idx + 1] = cell->fg.g / 255.0;
		atlas->cache_fgcol[idx + 2] = cell->fg.b / 255.0;
		atlas->cache_bgcol[idx + 0] = cell->bg.r / 255.0;
		atlas->cache_bgcol[idx + 1] = cell->bg.g / 255.0;
		atlas->cache_bgcol[idx + 2] = cell->bg.b / 255.0;
	}

	++atlas->cache_num;

	return 0;
}

static int gltex_draw(struct kmscon_text *txt, const struct tsm_screen_cell *cells,
		      struct kmscon_cursor *cursor)
{
	unsigned int posx, posy, off;

	for (posy = 0; posy < txt->rows; posy++) {
		for (posx = 0; posx < txt->cols; posx++) {
			off = posx + posy * txt->cols;

			if (cursor->visible && cursor->x == posx && cursor->y == posy)
				gltex_draw_cell(txt, &cursor->cell, posx, posy);
			else if (cells[off].attr2.blink && txt->blinking) {
				struct tsm_screen_cell cell = cells[off];

				cell.ch = ' ';
				gltex_draw_cell(txt, &cell, posx, posy);
			} else
				gltex_draw_cell(txt, &cells[off], posx, posy);
		}
	}
	return 0;
}

static int gltex_draw_pointer(struct kmscon_text *txt, unsigned int x, unsigned int y)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	struct gl_glyph *glyph;
	float gl_x1, gl_x2, gl_y1, gl_y2;
	unsigned int sw, sh;
	int i, idx;
	struct tsm_screen_cell pointer_cell = {0};
	pointer_cell.ch = 'I';

	glyph = find_glyph(txt, &pointer_cell);
	if (!glyph)
		return -ENOMEM;

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

	gl_x1 = gt->off_x + x * 2.0 / sw - 1.0 - gt->advance_x / 2.0;
	gl_y1 = 1.0 - gt->off_y - y * 2.0 / sh + gt->advance_y / 2.0;
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
		atlas->cache_fgcol[idx + 0] = gt->attr.fr / 255.0;
		atlas->cache_fgcol[idx + 1] = gt->attr.fg / 255.0;
		atlas->cache_fgcol[idx + 2] = gt->attr.fb / 255.0;
		atlas->cache_bgcol[idx + 0] = gt->attr.br / 255.0;
		atlas->cache_bgcol[idx + 1] = gt->attr.bg / 255.0;
		atlas->cache_bgcol[idx + 2] = gt->attr.bb / 255.0;
	}

	++atlas->cache_num;

	return 0;
}

static int gltex_render(struct kmscon_text *txt)
{
	struct gltex *gt = txt->data;
	struct atlas *atlas;
	float mat[16];

	gl_clear_error();

	/* Pass 1 only has to run when the console changed or the scene
	 * texture does not hold the current console. Render-only frames with
	 * post-processing just re-run the post-processing pass on the
	 * off-screen scene texture. Without post-processing the screen
	 * framebuffer has no memory, so always draw it. */
	if (txt->rebuild || !gt->post_shader || !gt->scene_valid) {
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

		gt->scene_valid = true;
	}

	if (gt->post_shader) {
		/* pass 2: draw the off-screen console through the shader.
		 * With ghosting the shader also blends the previous frame in
		 * (history + gamma uniforms) and the result written to the
		 * screen is copied into the history buffer afterwards. */
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, post_vertices);
		glViewport(0, 0, gt->sw, gt->sh);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		gl_shader_use(gt->post_shader);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, gt->scene.tex);
		glUniform1i(gt->post_uni_source, 0);
		glUniform1f(gt->post_uni_time, gltex_get_time(gt));
		glUniform2f(gt->post_uni_resolution, gt->sw, gt->sh);

		if (gt->ghost_gamma > 0) {
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, gt->hist.tex);
			glUniform1i(gt->post_uni_history, 1);
			glUniform1f(gt->post_uni_gamma, gt->ghost_gamma);
		}

		glDrawArrays(GL_TRIANGLES, 0, 6);

		if (gt->ghost_gamma > 0) {
			/* update the history buffer with the frame just drawn */
			glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, gt->sw, gt->sh);
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, 0);
			glActiveTexture(GL_TEXTURE0);
		}

		glDisableVertexAttribArray(0);
		glBindTexture(GL_TEXTURE_2D, 0);

		if (gl_has_error(gt->post_shader)) {
			log_warning("fragment shader failed; ignoring shader from now on");
			gltex_disable_post(gt);
		}
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
	.resize = gltex_resize,
	.rotate = gltex_rotate,
	.prepare = gltex_prepare,
	.draw = gltex_draw,
	.draw_pointer = gltex_draw_pointer,
	.render = gltex_render,
	.abort = NULL,
};
