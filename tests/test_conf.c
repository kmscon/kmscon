#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/misc/conf.c"
#include "config.h"
#include "render/text.h"
#undef LOG_SUBSYSTEM
#include "../src/config.c"

static char *config_file = "verbose\n"
			   "mode=pci-0000:0d:00.0[HDMI-1]1280x1024@60\n"
			   "mode=[DP-1]1024x768@120\n"
			   "mode=pci-0000:06:00.0[eDP]3440x1440\n"
			   "mode=[VGA-1]800x600\n"
			   "mode=1024x768@50\n"
			   "mode=640x480\n";

static struct kmscon_conf_mode parsed_modes[] = {
	{
		.gpu = "pci-0000:0d:00.0",
		.connector = "HDMI-1",
		.width = 1280,
		.height = 1024,
		.refresh_rate = 60,
	},
	{
		.gpu = "",
		.connector = "DP-1",
		.width = 1024,
		.height = 768,
		.refresh_rate = 120,
	},
	{
		.gpu = "pci-0000:06:00.0",
		.connector = "eDP",
		.width = 3440,
		.height = 1440,
		.refresh_rate = 0,
	},
	{
		.gpu = "",
		.connector = "VGA-1",
		.width = 800,
		.height = 600,
		.refresh_rate = 0,
	},
	{
		.gpu = "",
		.connector = "",
		.width = 1024,
		.height = 768,
		.refresh_rate = 50,
	},
	{
		.gpu = "",
		.connector = "",
		.width = 640,
		.height = 480,
		.refresh_rate = 0,
	},
};

void test_parse_mode(const char *config, struct kmscon_conf_mode *mode, int num_modes)
{
	struct conf_ctx *ctx;
	struct kmscon_conf_t *conf;
	int i;
	char *buf;

	kmscon_conf_new(&ctx);
	conf_ctx_reset(ctx);

	buf = malloc(strlen(config) + 1);
	strcpy(buf, config);

	parse_buffer(ctx->opts, ctx->onum, buf, strlen(buf));
	conf = conf_ctx_get_mem(ctx);
	free(buf);

	assert(conf->mode_count == num_modes);

	for (i = 0; i < num_modes; i++) {
		assert(strcmp(conf->modes[i].gpu, mode[i].gpu) == 0);
		assert(strcmp(conf->modes[i].connector, mode[i].connector) == 0);
		assert(conf->modes[i].width == mode[i].width);
		assert(conf->modes[i].height == mode[i].height);
		assert(conf->modes[i].refresh_rate == mode[i].refresh_rate);
	}
}

static char *screen_conf = "screen=normal\n"
			   "screen=pci-00d.00.0[HDMI-A-1]disabled\n"
			   "screen=[VGA-1]upside-down\n";

static struct kmscon_conf_screen parsed_screens[] = {
	{"", "", true, OR_NORMAL},
	{"pci-00d.00.0", "HDMI-A-1", false, OR_NORMAL},
	{"", "VGA-1", true, OR_UPSIDE_DOWN},
};

void test_parse_screen(const char *config, struct kmscon_conf_screen *screen, int num_screens)
{
	struct conf_ctx *ctx;
	struct kmscon_conf_t *conf;
	int i;
	char *buf;

	kmscon_conf_new(&ctx);
	conf_ctx_reset(ctx);

	buf = malloc(strlen(config) + 1);
	strcpy(buf, config);

	parse_buffer(ctx->opts, ctx->onum, buf, strlen(buf));
	conf = conf_ctx_get_mem(ctx);
	free(buf);

	assert(conf->screen_count == num_screens);

	for (i = 0; i < num_screens; i++) {
		assert(strcmp(conf->screens[i].gpu, screen[i].gpu) == 0);
		assert(strcmp(conf->screens[i].connector, screen[i].connector) == 0);
		assert(conf->screens[i].enabled == screen[i].enabled);
		assert(conf->screens[i].orientation == screen[i].orientation);
	}
}

int main()
{
	test_parse_mode(config_file, parsed_modes, 6);
	test_parse_screen(screen_conf, parsed_screens, 3);

	return 0;
}
