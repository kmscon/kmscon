/*
 * Lightweight test for kmscon_text_set / kmscon_text_unset.
 * We avoid linking the whole tree by stubbing external deps.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "../src/text.c" /* pull in kmscon_text_set without changing meson */

/* --- Stubs for external functions used by kmscon_text_set --- */
void kmscon_font_ref(struct kmscon_font *font) {}
void kmscon_font_unref(struct kmscon_font *font) {}
void uterm_display_ref(struct uterm_display *disp) {}
void uterm_display_unref(struct uterm_display *disp) {}

static int dummy_set_calls;
static int dummy_unset_calls;
static int dummy_set(struct kmscon_text *txt)
{
	dummy_set_calls++;
	return 0;
}
static void dummy_unset(struct kmscon_text *txt)
{
	dummy_unset_calls++;
}

static struct kmscon_text_ops dummy_ops = {
	.name = "dummytest",
	.owner = NULL,
	.init = NULL,
	.destroy = NULL,
	.set = dummy_set,
	.unset = dummy_unset,
};

int main(void)
{
	struct kmscon_text txt;
	struct kmscon_font fake_font;
	struct uterm_display *fake_disp = (struct uterm_display *)0x1;
	int ret;

	memset(&txt, 0, sizeof(txt));
	memset(&fake_font, 0, sizeof(fake_font));
	txt.ops = &dummy_ops;

	/* set calls backend set */
	ret = kmscon_text_set(&txt, &fake_font, &fake_font, fake_disp);
	assert(ret == 0);
	assert(dummy_set_calls == 1);
	assert(txt.font == &fake_font);
	assert(txt.disp == fake_disp);

	/* unset calls backend unset and clears pointers */
	kmscon_text_unset(&txt);
	assert(dummy_unset_calls == 1);
	assert(txt.font == NULL);
	assert(txt.disp == NULL);

	/* NULL font must return -EINVAL */
	ret = kmscon_text_set(&txt, NULL, NULL, fake_disp);
	assert(ret == -EINVAL);
	assert(dummy_set_calls == 1); /* not called again */

	return 0;
}
