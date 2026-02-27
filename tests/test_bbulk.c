/*
 * Lightweight test for repeated bbulk_set calls (no leaks, all cells re-damaged).
 * We include the implementation to access static helpers.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "text.h"

/* ---- Stubs for external dependencies used by text_bbulk.c ---- */
#include "../src/font.h"	/* for kmscon_font_* */
#include "../src/font_rotate.h" /* for prototypes and shl_hashtable */
#include "../src/uterm_video.h" /* for uterm_display_* prototypes */
unsigned int uterm_display_get_width(struct uterm_display *disp)
{
	(void)disp;
	return 640;
}
unsigned int uterm_display_get_height(struct uterm_display *disp)
{
	(void)disp;
	return 480;
}

/* Provide stub implementations matching the prototypes from font_rotate.h */
int kmscon_rotate_create_tables(struct shl_hashtable **normal, struct shl_hashtable **bold,
				void (*free_func)(void *data))
{
	(void)normal;
	(void)bold;
	(void)free_func;
	return 0;
}
void kmscon_rotate_free_tables(struct shl_hashtable *normal, struct shl_hashtable *bold)
{
	(void)normal;
	(void)bold;
}

/* Keep geometry tiny to minimize allocations */
#define FAKE_CELL_W 8
#define FAKE_CELL_H 16

/* Stub font metrics APIs used indirectly by text.c/text_bbulk.c */
unsigned int kmscon_font_get_width(const struct kmscon_font *font)
{
	(void)font;
	return FAKE_CELL_W;
}
unsigned int kmscon_font_get_height(const struct kmscon_font *font)
{
	(void)font;
	return FAKE_CELL_H;
}

/* Stub font rendering APIs used by text_bbulk.c */
int kmscon_font_render(struct kmscon_font *font, uint64_t id, const uint32_t *ch, size_t len,
		       const struct kmscon_glyph **out)
{
	static struct kmscon_glyph g;
	(void)font;
	(void)id;
	(void)ch;
	(void)len;
	memset(&g, 0, sizeof(g));
	g.buf.format = UTERM_FORMAT_XRGB32;
	g.buf.width = g.buf.height = FAKE_CELL_W;
	g.buf.stride = g.buf.width * 4;
	if (out)
		*out = &g;
	return 0;
}
int kmscon_font_render_inval(struct kmscon_font *font, const struct kmscon_glyph **out)
{
	return kmscon_font_render_empty(font, out);
}
int kmscon_font_render_empty(struct kmscon_font *font, const struct kmscon_glyph **out)
{
	static struct kmscon_glyph g;
	(void)font;
	memset(&g, 0, sizeof(g));
	g.buf.format = UTERM_FORMAT_XRGB32;
	g.buf.width = g.buf.height = FAKE_CELL_W;
	g.buf.stride = g.buf.width * 4;
	if (out)
		*out = &g;
	return 0;
}
int kmscon_rotate_glyph(struct kmscon_glyph *vb, const struct kmscon_glyph *glyph,
			enum Orientation o, uint8_t align)
{
	(void)o;
	(void)align;
	if (!vb || !glyph)
		return -EINVAL;
	*vb = *glyph;
	if (glyph->buf.stride == 0 || glyph->buf.height == 0)
		return 0;
	size_t buf_size = glyph->buf.stride * glyph->buf.height;
	vb->buf.data = malloc(buf_size);
	if (!vb->buf.data)
		return -ENOMEM;
	memset(vb->buf.data, 0x11, buf_size);
	return 0;
}

/* Stub uterm display APIs used by text_bbulk.c */
bool uterm_display_need_redraw(struct uterm_display *disp)
{
	(void)disp;
	return false;
}
bool uterm_display_has_damage(struct uterm_display *disp)
{
	(void)disp;
	return false;
}
bool uterm_display_supports_damage(struct uterm_display *disp)
{
	(void)disp;
	return true;
}
int uterm_display_fill(struct uterm_display *disp, uint8_t r, uint8_t g, uint8_t b, unsigned int x,
		       unsigned int y, unsigned int w, unsigned int h)
{
	(void)disp;
	(void)r;
	(void)g;
	(void)b;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	return 0;
}
int uterm_display_fake_blendv(struct uterm_display *disp, const struct uterm_video_blend_req *req,
			      size_t num)
{
	(void)disp;
	(void)req;
	(void)num;
	return 0;
}
void uterm_display_set_damage(struct uterm_display *disp, size_t n_rect,
			      struct uterm_video_rect *damages)
{
	(void)disp;
	(void)n_rect;
	(void)damages;
}

/* Pull in the implementation so we can call bbulk_set directly */
#include "../src/text_bbulk.c"

/* Fake font objects with valid width/height for FONT_WIDTH/FONT_HEIGHT macros */
static struct kmscon_font fake_font = {.attr = {.width = FAKE_CELL_W, .height = FAKE_CELL_H}};
static struct kmscon_font fake_bold = {.attr = {.width = FAKE_CELL_W, .height = FAKE_CELL_H}};

static void init_fake_txt(struct kmscon_text *txt)
{
	memset(txt, 0, sizeof(*txt));
	txt->ops = &kmscon_text_bbulk_ops;
	txt->disp = (struct uterm_display *)0x1; /* non-null stub */
	txt->font = &fake_font;
	txt->bold_font = &fake_bold;
	txt->orientation = OR_NORMAL;
	txt->ref = 1;
}

int main(void)
{
	struct kmscon_text txt;
	int ret;

	init_fake_txt(&txt);

	ret = kmscon_text_bbulk_ops.init(&txt);
	assert(ret == 0);
	struct bbulk *bb = txt.data;
	assert(bb != NULL);

	/* First call allocates */
	ret = bbulk_set(&txt);
	assert(ret == 0);
	assert(bb->reqs && bb->prev && bb->damages && bb->damage_rects);
	unsigned int prev_cells = bb->cells;

	/* Second call with identical geometry should remain valid and fully damaged */
	ret = bbulk_set(&txt);
	assert(ret == 0);
	assert(bb->cells == prev_cells);
	assert(bb->reqs != NULL);
	assert(bb->prev != NULL);
	assert(bb->damages != NULL);
	assert(bb->damage_rects != NULL);
	/* All cells should be marked damaged */
	for (unsigned i = 0; i < bb->cells; ++i)
		assert(bb->prev[i].id == ID_DAMAGED);

	/* Exercise prepare/render + damage path */
	struct tsm_screen_attr attr;
	memset(&attr, 0, sizeof(attr));
	ret = bbulk_prepare(&txt, &attr);
	assert(ret == 0);
	ret = bbulk_render(&txt);
	assert(ret == 0);
	assert(bb->damage_rect_len > 0);

	bbulk_unset(&txt);
	assert(bb->reqs == NULL);
	assert(bb->prev == NULL);
	assert(bb->damages == NULL);
	assert(bb->damage_rects == NULL);
	kmscon_text_bbulk_ops.destroy(&txt);
	return 0;
}
