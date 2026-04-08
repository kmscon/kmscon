/*
 * kmscon - /etc/issue display
 *
 * Copyright (c) 2025 Alberto Ruiz <aruiz@redhat.com>
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

#include <glob.h>
#include <libtsm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include "kmscon_issue.h"
#include "pty.h"

/* Cap total collected issue text to prevent runaway allocation. */
#define ISSUE_MAX_SIZE (256u * 1024u)

/*
 * Default search path, matching agetty's default:
 *   /etc/issue:/etc/issue.d
 *
 * Distros can override this with --issue-path or the issue-path config
 * option using a colon-separated list.  Plain files are read directly;
 * directories are scanned for *.issue files in lexicographic order.
 */
#define ISSUE_DEFAULT_PATH "/etc/issue:/etc/issue.d"

static char *read_os_release_field(const char *field)
{
	FILE *fp;
	char line[512];
	size_t flen;

	fp = fopen("/etc/os-release", "r");
	if (!fp)
		fp = fopen("/usr/lib/os-release", "r");
	if (!fp)
		return NULL;

	flen = strlen(field);
	if (flen >= sizeof(line)) {
		fclose(fp);
		return NULL;
	}
	while (fgets(line, sizeof(line), fp)) {
		char *val;
		size_t vlen;

		if (strncmp(line, field, flen) != 0 || line[flen] != '=')
			continue;

		val = &line[flen + 1];
		vlen = strlen(val);
		if (vlen > 0 && val[vlen - 1] == '\n')
			val[--vlen] = '\0';
		if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
			val++;
			val[vlen - 2] = '\0';
		}
		fclose(fp);
		return strdup(val);
	}

	fclose(fp);
	return NULL;
}

static size_t append_file(FILE *out, const char *path, size_t limit)
{
	FILE *fp;
	char buf[4096];
	size_t n, total = 0;

	if (!limit)
		return 0;

	fp = fopen(path, "r");
	if (!fp)
		return 0;

	while (total < limit) {
		size_t chunk = sizeof(buf);
		if (chunk > limit - total)
			chunk = limit - total;
		n = fread(buf, 1, chunk, fp);
		if (!n)
			break;
		fwrite(buf, 1, n, out);
		total += n;
	}

	fclose(fp);
	return total;
}

static size_t append_dir(FILE *out, const char *dir, size_t limit)
{
	glob_t gl;
	char *pattern;
	size_t i, total = 0;

	if (asprintf(&pattern, "%s/*.issue", dir) < 0)
		return 0;

	if (glob(pattern, 0, NULL, &gl) != 0) {
		free(pattern);
		return 0;
	}
	free(pattern);

	for (i = 0; i < gl.gl_pathc && total < limit; i++)
		total += append_file(out, gl.gl_pathv[i], limit - total);

	globfree(&gl);
	return total;
}

/*
 * Collect raw issue text from all search path entries into a single buffer.
 * @search_path is a colon-separated list of files and directories.
 * Returns a malloc'd string (caller frees) or NULL if nothing was found.
 */
static char *collect_issue_text(const char *search_path, size_t *out_len)
{
	FILE *mem;
	char *buf = NULL;
	size_t len = 0, total = 0;
	char *dup, *saveptr, *entry;

	if (!search_path || !*search_path)
		return NULL;

	mem = open_memstream(&buf, &len);
	if (!mem)
		return NULL;

	dup = strdup(search_path);
	if (!dup) {
		fclose(mem);
		return NULL;
	}

	for (entry = strtok_r(dup, ":", &saveptr); entry && total < ISSUE_MAX_SIZE;
	     entry = strtok_r(NULL, ":", &saveptr)) {
		size_t n = append_file(mem, entry, ISSUE_MAX_SIZE - total);
		if (!n)
			n = append_dir(mem, entry, ISSUE_MAX_SIZE - total);
		total += n;
	}

	free(dup);
	fclose(mem);

	if (!len) {
		free(buf);
		return NULL;
	}

	*out_len = len;
	return buf;
}

static void expand_os_release(const char **ppos, const char *end, FILE *out)
{
	const char *pos = *ppos;
	char *field_val;

	if (pos < end && *pos == '{') {
		char field_name[64];
		char *fe = field_name;
		pos++;
		while (pos < end && *pos != '}' && fe < field_name + sizeof(field_name) - 1)
			*fe++ = *pos++;
		*fe = '\0';
		while (pos < end && *pos != '}')
			pos++;
		if (pos < end && *pos == '}')
			pos++;
		field_val = read_os_release_field(field_name);
	} else {
		field_val = read_os_release_field("PRETTY_NAME");
	}

	if (field_val) {
		fputs(field_val, out);
		free(field_val);
	}
	*ppos = pos;
}

/* Write literal text to out, converting \n to \r\n for the terminal. */
static void write_literal(FILE *out, const char *buf, size_t len)
{
	const char *pos = buf, *end = buf + len, *nl;

	while (pos < end) {
		nl = memchr(pos, '\n', end - pos);
		if (!nl) {
			fwrite(pos, 1, end - pos, out);
			break;
		}
		fwrite(pos, 1, nl - pos, out);
		fputs("\r\n", out);
		pos = nl + 1;
	}
}

static void expand_issue(const char *raw, size_t raw_len, struct tsm_vte *vte,
			 struct kmscon_pty *pty)
{
	FILE *out;
	char *obuf = NULL;
	size_t olen = 0;
	struct utsname uts;
	struct tm *tm;
	time_t now;
	const char *pos, *end, *esc;
	char timebuf[64];
	char datebuf[64];
	char tty_name[128];
	const char *tty_short;
	const char *escape_val[128] = {0};

	out = open_memstream(&obuf, &olen);
	if (!out)
		return;

	if (uname(&uts) < 0)
		memset(&uts, 0, sizeof(uts));

	now = time(NULL);
	tm = localtime(&now);
	if (tm) {
		strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);
		strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", tm);
	} else {
		timebuf[0] = '\0';
		datebuf[0] = '\0';
	}

	tty_name[0] = '\0';
	kmscon_pty_get_slave_name(pty, tty_name, sizeof(tty_name));
	tty_short = tty_name;
	if (!strncmp(tty_short, "/dev/", 5))
		tty_short += 5;

	escape_val['\\'] = "\\";
	escape_val['e'] = "\033";
	escape_val['s'] = uts.sysname;
	escape_val['n'] = uts.nodename;
	escape_val['r'] = uts.release;
	escape_val['v'] = uts.version;
	escape_val['m'] = uts.machine;
	escape_val['o'] = uts.domainname[0] ? uts.domainname : "(none)";
	escape_val['d'] = datebuf;
	escape_val['t'] = timebuf;
	escape_val['l'] = tty_short;

	pos = raw;
	end = raw + raw_len;
	while (pos < end) {
		esc = memchr(pos, '\\', end - pos);
		if (!esc) {
			write_literal(out, pos, end - pos);
			break;
		}

		write_literal(out, pos, esc - pos);
		pos = esc + 1;
		if (pos >= end)
			break;

		if (*pos == 'S') {
			pos++;
			expand_os_release(&pos, end, out);
			continue;
		}

		if ((unsigned char)*pos < 128 && escape_val[(unsigned char)*pos])
			fputs(escape_val[(unsigned char)*pos], out);
		pos++;
	}

	fclose(out);
	if (olen > 0)
		tsm_vte_input(vte, obuf, olen);
	free(obuf);
}

void kmscon_issue_write(struct tsm_vte *vte, struct kmscon_pty *pty, const char *search_path)
{
	char *raw;
	size_t raw_len;

	if (!search_path)
		search_path = ISSUE_DEFAULT_PATH;

	raw = collect_issue_text(search_path, &raw_len);
	if (!raw)
		return;

	expand_issue(raw, raw_len, vte, pty);
	free(raw);
}
