/*
 * The fbdev backend's blend, in each pixel format it writes: a blend over the screen leaves
 * a pixel the buffer does not cover as it is, an ordinary blend gives it the background.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Pull in the implementation so the blend runs on a framebuffer in memory */
#include "../src/video/fbdev_render.c"

struct format {
	unsigned int Bpp;
	bool xrgb32;
	unsigned int off_r, off_g, off_b;
	unsigned int len_r, len_g, len_b;
};

static const struct format formats[] = {
	{4, true, 16, 8, 0, 8, 8, 8},
	{4, false, 0, 8, 16, 8, 8, 8},
	{3, false, 16, 8, 0, 8, 8, 8},
	{2, false, 11, 5, 0, 5, 6, 5},
};

#define WIDTH 3

static uint_fast32_t read_pixel(const struct fbdev_display *fbdev, unsigned int i)
{
	if (fbdev->Bpp == 2)
		return ((uint16_t *)fbdev->map)[i];
	if (fbdev->Bpp == 3)
		return read_24bit(&fbdev->map[i * 3]);
	return ((uint32_t *)fbdev->map)[i];
}

static void run(const struct format *f, bool over, uint_fast32_t out[WIDTH])
{
	struct fbdev_display fbdev = {0};
	struct display disp = {0};
	struct video_buffer *buf = calloc(1, sizeof(*buf) + WIDTH);
	struct video_blend_req req = {0};
	uint32_t store[WIDTH] = {0};
	uint_fast32_t shown;
	unsigned int i;

	fbdev.Bpp = f->Bpp;
	fbdev.xrgb32 = f->xrgb32;
	fbdev.off_r = f->off_r;
	fbdev.off_g = f->off_g;
	fbdev.off_b = f->off_b;
	fbdev.len_r = f->len_r;
	fbdev.len_g = f->len_g;
	fbdev.len_b = f->len_b;
	fbdev.xres = WIDTH;
	fbdev.yres = 1;
	fbdev.stride = WIDTH * f->Bpp;
	fbdev.map = (uint8_t *)store;
	disp.data = &fbdev;

	/* the screen shows red */
	shown = xrgb32_to_device(&disp, 0xff0000);
	for (i = 0; i < WIDTH; i++) {
		if (f->Bpp == 2)
			((uint16_t *)store)[i] = shown;
		else if (f->Bpp == 3)
			write_24bit(&fbdev.map[i * 3], shown);
		else
			store[i] = shown;
	}

	assert(buf);
	buf->width = WIDTH;
	buf->height = 1;
	buf->data[0] = 0;
	buf->data[1] = 255;
	buf->data[2] = 128;

	/* white on a blue background */
	req.buf = buf;
	req.w = WIDTH;
	req.h = 1;
	req.fr = req.fg = req.fb = 255;
	req.bb = 255;
	req.over = over;
	assert(fbdev_display_blend(&disp, &req) == 0);

	for (i = 0; i < WIDTH; i++)
		out[i] = read_pixel(&fbdev, i);
	free(buf);
}

int main(void)
{
	unsigned int n;

	for (n = 0; n < sizeof(formats) / sizeof(*formats); n++) {
		const struct format *f = &formats[n];
		struct fbdev_display fbdev = {.off_r = f->off_r,
					      .off_g = f->off_g,
					      .off_b = f->off_b,
					      .len_r = f->len_r,
					      .len_g = f->len_g,
					      .len_b = f->len_b};
		struct display disp = {.data = &fbdev};
		uint_fast32_t red = xrgb32_to_device(&disp, 0xff0000);
		uint_fast32_t blue = xrgb32_to_device(&disp, 0x0000ff);
		uint_fast32_t white = xrgb32_to_device(&disp, 0xffffff);
		uint_fast32_t out[WIDTH];

		run(f, true, out);
		assert(out[0] == red);
		assert(out[1] == white);
		/* half white over red: the red stays full, green and blue come half way */
		assert(device_channel(out[2], f->off_r, f->len_r) == 255);
		assert(device_channel(out[2], f->off_g, f->len_g) > 100);
		assert(device_channel(out[2], f->off_g, f->len_g) < 160);
		assert(device_channel(out[2], f->off_b, f->len_b) > 100);
		assert(device_channel(out[2], f->off_b, f->len_b) < 160);

		run(f, false, out);
		assert(out[0] == blue);
		assert(out[1] == white);
		assert(device_channel(out[2], f->off_r, f->len_r) > 100);
		assert(device_channel(out[2], f->off_r, f->len_r) < 160);
		assert(device_channel(out[2], f->off_b, f->len_b) >= 247);
	}
	return 0;
}
