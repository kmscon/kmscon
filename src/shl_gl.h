/*
 * shl - OpenGL Helpers
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
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
 * OpenGL Helpers
 * This file provides several helper functions that are commonly used when
 * working with OpenGL.
 * TODO: Rename to shl_gl_* prefix.
 */

#ifndef SHL_GL_H
#define SHL_GL_H

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

static inline void gl_m4_identity(float *m)
{
	if (!m)
		return;

	m[0] = 1;
	m[1] = 0;
	m[2] = 0;
	m[3] = 0;

	m[4] = 0;
	m[5] = 1;
	m[6] = 0;
	m[7] = 0;

	m[8] = 0;
	m[9] = 0;
	m[10] = 1;
	m[11] = 0;

	m[12] = 0;
	m[13] = 0;
	m[14] = 0;
	m[15] = 1;
}

/*
 * Shader Helpers
 * These helpers load, compile and link shaders and allow easy attribute/uniform
 * access.
 */

struct gl_shader;

int gl_shader_new(struct gl_shader **out, const char *vert, int vert_len, const char *frag,
		  int frag_len, char **attr, size_t attr_count);
void gl_shader_ref(struct gl_shader *shader);
void gl_shader_unref(struct gl_shader *shader);
GLuint gl_shader_get_uniform(struct gl_shader *shader, const char *name);
void gl_shader_use(struct gl_shader *shader);

void gl_tex_new(GLuint *tex, size_t num);
void gl_tex_free(GLuint *tex, size_t num);
void gl_tex_load(GLuint tex, unsigned int width, unsigned int stride, unsigned int height,
		 uint8_t *buf);

void gl_clear_error();
bool gl_has_error(struct gl_shader *shader);
const char *gl_err_to_str(GLenum err);

#endif /* SHL_GL_H */
