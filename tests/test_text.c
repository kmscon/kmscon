/*
 * Lightweight test for render_set / render_unset.
 * We avoid linking the whole tree by stubbing external deps.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "../src/render/render.c" /* pull in render_set without changing meson */
#include "video/video.h"

/* --- Stubs for external functions used by render_set --- */
void kmscon_font_ref(struct kmscon_font *font) {}
void kmscon_font_unref(struct kmscon_font *font) {}
void display_ref(struct display *disp) {}
void display_unref(struct display *disp) {}
int display_show_cursor(struct display *disp, int32_t x, int32_t y)
{
	return 0;
}
unsigned int display_get_height(struct display *disp)
{
	return 0;
}
unsigned int display_get_width(struct display *disp)
{
	return 0;
}

static int dummy_set_calls;
static int dummy_unset_calls;
static int dummy_set(struct render *txt)
{
	dummy_set_calls++;
	return 0;
}
static void dummy_unset(struct render *txt)
{
	dummy_unset_calls++;
}

static struct render_ops dummy_ops = {
	.name = "dummytest",
	.owner = NULL,
	.init = NULL,
	.destroy = NULL,
	.set = dummy_set,
	.unset = dummy_unset,
};

int main(void)
{
	struct render txt = {0};
	struct kmscon_font fake_font = {0};
	int ret;

	txt.ops = &dummy_ops;

	/* set calls backend set */
	ret = render_set(&txt, &fake_font);
	assert(ret == 0);
	assert(dummy_set_calls == 1);
	assert(txt.font == &fake_font);

	/* unset calls backend unset and clears pointers */
	render_unset(&txt);
	assert(dummy_unset_calls == 1);
	assert(txt.font == NULL);
	assert(txt.disp == NULL);

	/* NULL font must return -EINVAL */
	ret = render_set(&txt, NULL);
	assert(ret == -EINVAL);
	assert(dummy_set_calls == 1); /* not called again */

	return 0;
}
