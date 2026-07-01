/*
 * kmscon - asciicast playback for terminal sessions
 */

#include <ctype.h>
#include <errno.h>
#include <libtsm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "asciinema.h"
#include "shl/eloop.h"
#include "shl/log.h"

#define LOG_SUBSYSTEM "asciinema"

#define ASCIINEMA_MAX_LINE (256u * 1024u)
#define ASCIINEMA_MAX_OUTPUT (2u * 1024u * 1024u)
#define ASCIINEMA_MAX_EVENTS 16384u
#define ASCIINEMA_MIN_TIMER_NS (1 * 1000 * 1000)

struct asciinema_event {
	uint64_t delay_ns;
	char *data;
	size_t len;
};

struct kmscon_asciinema {
	struct asciinema_event *events;
	size_t num;
	size_t pos;
	bool loop;
	bool stopped;
	struct ev_timer *timer;
	kmscon_asciinema_write_cb write_cb;
	void *data;
};

static const char *skip_ws(const char *p)
{
	while (*p && isspace((unsigned char)*p))
		p++;
	return p;
}

static int hex_val(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_hex4(const char *p, uint32_t *out)
{
	uint32_t v = 0;
	int h;

	for (size_t i = 0; i < 4; i++) {
		h = hex_val(p[i]);
		if (h < 0)
			return -EINVAL;
		v = (v << 4) | (uint32_t)h;
	}

	*out = v;
	return 0;
}

static int append_utf8(char **dst, char *end, uint32_t cp)
{
	char *p = *dst;
	char u8[4];
	size_t len;

	if (cp > 0x10ffff)
		return -EINVAL;

	len = tsm_ucs4_to_utf8(cp, u8);
	if (!len || len > sizeof(u8))
		return -EINVAL;
	if (p + len > end)
		return -ENOSPC;

	memcpy(p, u8, len);
	p += len;

	*dst = p;
	return 0;
}

static int parse_json_string(const char **ppos, char **out, size_t *out_len)
{
	const char *pos = skip_ws(*ppos);
	const char *src;
	char *buf, *dst, *end;
	size_t alloc;
	uint32_t cp;
	int ret;

	if (*pos != '"')
		return -EINVAL;

	src = ++pos;
	while (*pos && *pos != '"') {
		if (*pos == '\\' && pos[1])
			pos++;
		pos++;
	}
	if (*pos != '"')
		return -EINVAL;

	alloc = (size_t)(pos - src) + 1;
	buf = malloc(alloc);
	if (!buf)
		return -ENOMEM;

	dst = buf;
	end = buf + alloc;
	pos = src;
	while (*pos && *pos != '"') {
		if (*pos != '\\') {
			if (dst + 1 > end) {
				ret = -ENOSPC;
				goto err;
			}
			*dst++ = *pos++;
			continue;
		}

		pos++;
		switch (*pos) {
		case '"':
		case '\\':
		case '/':
			*dst++ = *pos++;
			break;
		case 'b':
			*dst++ = '\b';
			pos++;
			break;
		case 'f':
			*dst++ = '\f';
			pos++;
			break;
		case 'n':
			*dst++ = '\n';
			pos++;
			break;
		case 'r':
			*dst++ = '\r';
			pos++;
			break;
		case 't':
			*dst++ = '\t';
			pos++;
			break;
		case 'u':
			ret = parse_hex4(pos + 1, &cp);
			if (ret)
				goto err;
			pos += 5;

			if (cp >= 0xd800 && cp <= 0xdbff && pos[0] == '\\' && pos[1] == 'u') {
				uint32_t low;

				ret = parse_hex4(pos + 2, &low);
				if (ret)
					goto err;
				if (low >= 0xdc00 && low <= 0xdfff) {
					cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
					pos += 6;
				}
			}

			ret = append_utf8(&dst, end, cp);
			if (ret)
				goto err;
			break;
		default:
			ret = -EINVAL;
			goto err;
		}
	}

	*out = buf;
	*out_len = (size_t)(dst - buf);
	*ppos = pos + 1;
	return 0;

err:
	free(buf);
	return ret;
}

static int parse_header_version(const char *line)
{
	const char *p = strstr(line, "\"version\"");

	if (!p)
		return -EINVAL;

	p = strchr(p, ':');
	if (!p)
		return -EINVAL;
	p = skip_ws(p + 1);
	return (int)strtol(p, NULL, 10);
}

static uint64_t sec_to_ns(double sec)
{
	if (sec <= 0)
		return 0;
	if (sec > 3600.0)
		sec = 3600.0;
	return (uint64_t)(sec * 1000000000.0);
}

static int add_event(struct kmscon_asciinema *player, uint64_t delay_ns, char *data, size_t len,
		     size_t *total_output)
{
	struct asciinema_event *events;

	if (player->num >= ASCIINEMA_MAX_EVENTS)
		return -E2BIG;
	if (len > ASCIINEMA_MAX_OUTPUT || *total_output > ASCIINEMA_MAX_OUTPUT - len)
		return -E2BIG;

	events = realloc(player->events, (player->num + 1) * sizeof(*events));
	if (!events)
		return -ENOMEM;

	player->events = events;
	player->events[player->num].delay_ns = delay_ns;
	player->events[player->num].data = data;
	player->events[player->num].len = len;
	player->num++;
	*total_output += len;
	return 0;
}

static void schedule_event(struct kmscon_asciinema *player, uint64_t delay_ns)
{
	struct itimerspec spec = {0};

	if (!player || !player->timer || player->stopped)
		return;

	if (delay_ns < ASCIINEMA_MIN_TIMER_NS)
		delay_ns = ASCIINEMA_MIN_TIMER_NS;

	spec.it_value.tv_sec = delay_ns / 1000000000ULL;
	spec.it_value.tv_nsec = delay_ns % 1000000000ULL;
	ev_timer_update(player->timer, &spec);
	ev_timer_enable(player->timer);
}

static void timer_event(struct ev_timer *timer, uint64_t count, void *data)
{
	struct kmscon_asciinema *player = data;
	uint64_t delay_ns;

	if (!player || player->stopped)
		return;

	if (kmscon_asciinema_advance(player, &delay_ns))
		schedule_event(player, delay_ns);
	else if (player->timer)
		ev_timer_disable(player->timer);
}

static int parse_event_line(struct kmscon_asciinema *player, const char *line, int version,
			    double *last_output_time, double *pending_interval,
			    size_t *total_output)
{
	const char *pos = skip_ws(line);
	char *code = NULL;
	char *data = NULL;
	size_t code_len = 0, data_len = 0;
	double t, delay_sec;
	uint64_t delay_ns;
	int ret = 0;

	if (*pos != '[')
		return 0;
	pos++;
	errno = 0;
	t = strtod(pos, (char **)&pos);
	if (errno)
		return -EINVAL;

	pos = skip_ws(pos);
	if (*pos != ',')
		return -EINVAL;
	pos++;

	ret = parse_json_string(&pos, &code, &code_len);
	if (ret)
		return ret;

	pos = skip_ws(pos);
	if (*pos != ',') {
		ret = -EINVAL;
		goto out;
	}
	pos++;

	ret = parse_json_string(&pos, &data, &data_len);
	if (ret)
		goto out;

	if (code_len != 1 || code[0] != 'o') {
		if (version == 3)
			*pending_interval += t;
		goto out;
	}

	if (version == 3) {
		delay_sec = *pending_interval + t;
		*pending_interval = 0;
	} else {
		delay_sec = t - *last_output_time;
		if (delay_sec < 0)
			delay_sec = 0;
		*last_output_time = t;
	}

	delay_ns = sec_to_ns(delay_sec);
	ret = add_event(player, delay_ns, data, data_len, total_output);
	if (ret)
		goto out;
	data = NULL;

out:
	free(code);
	free(data);
	return ret;
}

int kmscon_asciinema_new(struct kmscon_asciinema **out, struct ev_eloop *eloop, const char *path,
			 bool loop, kmscon_asciinema_write_cb write_cb, void *data)
{
	struct kmscon_asciinema *player;
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	FILE *fp;
	int version, ret = 0;
	double last_output_time = 0;
	double pending_interval = 0;
	size_t total_output = 0;

	if (!out || !eloop || !path || !write_cb)
		return -EINVAL;
	*out = NULL;

	fp = fopen(path, "r");
	if (!fp)
		return -errno;

	len = getline(&line, &cap, fp);
	if (len < 0) {
		ret = -EINVAL;
		goto out_file;
	}
	if ((size_t)len > ASCIINEMA_MAX_LINE) {
		ret = -E2BIG;
		goto out_file;
	}

	version = parse_header_version(line);
	if (version != 2 && version != 3) {
		ret = -EINVAL;
		goto out_file;
	}

	player = malloc(sizeof(*player));
	if (!player) {
		ret = -ENOMEM;
		goto out_file;
	}
	memset(player, 0, sizeof(*player));
	player->loop = loop;
	player->write_cb = write_cb;
	player->data = data;

	while ((len = getline(&line, &cap, fp)) >= 0) {
		if ((size_t)len > ASCIINEMA_MAX_LINE) {
			ret = -E2BIG;
			break;
		}
		if (version == 3 && line[0] == '#')
			continue;
		ret = parse_event_line(player, line, version, &last_output_time, &pending_interval,
				       &total_output);
		if (ret)
			break;
	}

	if (!ret && !player->num)
		ret = -ENODATA;
	if (!ret)
		ret = ev_eloop_new_timer(eloop, &player->timer, NULL, timer_event, player);
	if (ret) {
		kmscon_asciinema_free(player);
		goto out_file;
	}

	log_debug("loaded asciicast %s with %zu output events", path, player->num);
	*out = player;

out_file:
	free(line);
	fclose(fp);
	return ret;
}

void kmscon_asciinema_free(struct kmscon_asciinema *player)
{
	if (!player)
		return;

	for (size_t i = 0; i < player->num; i++)
		free(player->events[i].data);
	if (player->timer)
		ev_eloop_rm_timer(player->timer);
	free(player->events);
	free(player);
}

bool kmscon_asciinema_start(struct kmscon_asciinema *player)
{
	if (!player || !player->num)
		return false;

	player->pos = 0;
	player->stopped = false;
	schedule_event(player, player->events[0].delay_ns);
	return true;
}

bool kmscon_asciinema_advance(struct kmscon_asciinema *player, uint64_t *delay_ns)
{
	if (!player || !delay_ns || player->stopped || !player->num)
		return false;

	if (player->pos >= player->num) {
		if (!player->loop)
			return false;
		player->pos = 0;
	}

	do {
		struct asciinema_event *ev = &player->events[player->pos++];

		player->write_cb(ev->data, ev->len, player->data);
	} while (player->pos < player->num && player->events[player->pos].delay_ns == 0);

	if (player->pos >= player->num) {
		if (!player->loop)
			return false;
		player->pos = 0;
	}

	*delay_ns = player->events[player->pos].delay_ns;
	return true;
}

void kmscon_asciinema_pause(struct kmscon_asciinema *player)
{
	if (player && player->timer)
		ev_timer_disable(player->timer);
}

void kmscon_asciinema_resume(struct kmscon_asciinema *player)
{
	if (player && player->timer && !player->stopped)
		ev_timer_enable(player->timer);
}

void kmscon_asciinema_stop(struct kmscon_asciinema *player)
{
	if (player) {
		player->stopped = true;
		kmscon_asciinema_pause(player);
	}
}
