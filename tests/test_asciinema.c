/*
 * Lightweight tests for the asciicast player.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "asciinema.h"
#include "shl/eloop.h"

struct sink {
	char buf[4096];
	size_t len;
	unsigned int calls;
};

static void sink_write(const char *u8, size_t len, void *data)
{
	struct sink *sink = data;

	assert(sink->len + len < sizeof(sink->buf));
	memcpy(sink->buf + sink->len, u8, len);
	sink->len += len;
	sink->buf[sink->len] = '\0';
	sink->calls++;
}

static void assert_ns_near(uint64_t got, uint64_t expected)
{
	uint64_t delta = got > expected ? got - expected : expected - got;

	assert(delta < 1000);
}

static void write_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);

		assert(n > 0);
		buf += n;
		len -= (size_t)n;
	}
}

static char *write_tmp(const char *content)
{
	char template[] = "/tmp/kmscon-asciinema-XXXXXX";
	char *path;
	int fd;

	fd = mkstemp(template);
	assert(fd >= 0);
	write_all(fd, content, strlen(content));
	assert(close(fd) == 0);

	path = strdup(template);
	assert(path);
	return path;
}

static char *write_large_event_cast(unsigned int events)
{
	char template[] = "/tmp/kmscon-asciinema-XXXXXX";
	FILE *fp;
	char *path;
	int fd;

	fd = mkstemp(template);
	assert(fd >= 0);
	fp = fdopen(fd, "w");
	assert(fp);

	assert(fputs("{\"version\":2,\"width\":80,\"height\":24}\n", fp) >= 0);
	for (unsigned int i = 0; i < events; i++)
		assert(fputs("[0,\"o\",\"x\"]\n", fp) >= 0);
	assert(fclose(fp) == 0);

	path = strdup(template);
	assert(path);
	return path;
}

static void unlink_free(char *path)
{
	unlink(path);
	free(path);
}

static struct ev_eloop *new_eloop(void)
{
	struct ev_eloop *eloop = NULL;
	int ret;

	ret = ev_eloop_new(&eloop);
	assert(ret == 0);
	assert(eloop);
	return eloop;
}

static void test_v2_output_and_json_escapes(void)
{
	static const char cast[] = "{\"version\":2,\"width\":80,\"height\":24}\n"
				   "[0.1,\"o\",\"A\"]\n"
				   "[0.2,\"i\",\"ignored\"]\n"
				   "[0.4,\"o\",\"B\\n\\u001b[31m\\ud83d\\ude03\"]\n";
	struct kmscon_asciinema *player = NULL;
	struct ev_eloop *eloop;
	struct sink sink = {0};
	uint64_t delay_ns;
	char *path;
	bool more;
	int ret;

	path = write_tmp(cast);
	eloop = new_eloop();
	ret = kmscon_asciinema_new(&player, eloop, path, false, sink_write, &sink);
	unlink_free(path);
	assert(ret == 0);
	assert(player);

	assert(kmscon_asciinema_start(player));

	more = kmscon_asciinema_advance(player, &delay_ns);
	assert(more);
	assert_ns_near(delay_ns, 300000000ULL);
	assert(sink.calls == 1);
	assert(strcmp(sink.buf, "A") == 0);

	more = kmscon_asciinema_advance(player, &delay_ns);
	assert(!more);
	assert(sink.calls == 2);
	assert(strcmp(sink.buf, "AB\n\033[31m\xf0\x9f\x98\x83") == 0);

	kmscon_asciinema_free(player);
	ev_eloop_unref(eloop);
}

static void test_v3_intervals_and_ignored_events(void)
{
	static const char cast[] = "{\"version\":3,\"width\":80,\"height\":24}\n"
				   "[0.1,\"o\",\"A\"]\n"
				   "[0.2,\"m\",\"marker\"]\n"
				   "[0.3,\"o\",\"B\"]\n";
	struct kmscon_asciinema *player = NULL;
	struct ev_eloop *eloop;
	struct sink sink = {0};
	uint64_t delay_ns;
	char *path;
	bool more;
	int ret;

	path = write_tmp(cast);
	eloop = new_eloop();
	ret = kmscon_asciinema_new(&player, eloop, path, false, sink_write, &sink);
	unlink_free(path);
	assert(ret == 0);

	assert(kmscon_asciinema_start(player));
	more = kmscon_asciinema_advance(player, &delay_ns);
	assert(more);
	assert_ns_near(delay_ns, 500000000ULL);
	assert(strcmp(sink.buf, "A") == 0);

	more = kmscon_asciinema_advance(player, &delay_ns);
	assert(!more);
	assert(strcmp(sink.buf, "AB") == 0);

	kmscon_asciinema_free(player);
	ev_eloop_unref(eloop);
}

static void test_loop_and_stop(void)
{
	static const char cast[] = "{\"version\":2,\"width\":80,\"height\":24}\n"
				   "[0,\"o\",\"A\"]\n";
	struct kmscon_asciinema *player = NULL;
	struct ev_eloop *eloop;
	struct sink sink = {0};
	uint64_t delay_ns;
	char *path;
	int ret;

	path = write_tmp(cast);
	eloop = new_eloop();
	ret = kmscon_asciinema_new(&player, eloop, path, true, sink_write, &sink);
	unlink_free(path);
	assert(ret == 0);

	assert(kmscon_asciinema_start(player));
	assert(kmscon_asciinema_advance(player, &delay_ns));
	assert(strcmp(sink.buf, "A") == 0);

	kmscon_asciinema_stop(player);
	assert(!kmscon_asciinema_advance(player, &delay_ns));
	assert(strcmp(sink.buf, "A") == 0);

	kmscon_asciinema_free(player);
	ev_eloop_unref(eloop);
}

static void test_invalid_casts(void)
{
	struct kmscon_asciinema *player = NULL;
	struct ev_eloop *eloop;
	struct sink sink = {0};
	char *path;
	int ret;

	eloop = new_eloop();
	path = write_tmp("{\"version\":1,\"width\":80,\"height\":24}\n[0,\"o\",\"A\"]\n");
	ret = kmscon_asciinema_new(&player, eloop, path, false, sink_write, &sink);
	unlink_free(path);
	assert(ret == -EINVAL);
	assert(player == NULL);

	path = write_tmp("{\"version\":2,\"width\":80,\"height\":24}\n[0,\"o\",\"\\q\"]\n");
	ret = kmscon_asciinema_new(&player, eloop, path, false, sink_write, &sink);
	unlink_free(path);
	assert(ret == -EINVAL);
	assert(player == NULL);

	path = write_tmp("{\"version\":2,\"width\":80,\"height\":24}\n[0,\"o\",\"\\ud800\"]\n");
	ret = kmscon_asciinema_new(&player, eloop, path, false, sink_write, &sink);
	unlink_free(path);
	assert(ret == -EINVAL);
	assert(player == NULL);
	ev_eloop_unref(eloop);
}

static void test_event_limit(void)
{
	struct kmscon_asciinema *player = NULL;
	struct ev_eloop *eloop;
	struct sink sink = {0};
	char *path;
	int ret;

	eloop = new_eloop();
	path = write_large_event_cast(16385);
	ret = kmscon_asciinema_new(&player, eloop, path, false, sink_write, &sink);
	unlink_free(path);
	assert(ret == -E2BIG);
	assert(player == NULL);
	ev_eloop_unref(eloop);
}

int main(void)
{
	test_v2_output_and_json_escapes();
	test_v3_intervals_and_ignored_events();
	test_loop_and_stop();
	test_invalid_casts();
	test_event_limit();
	return 0;
}
