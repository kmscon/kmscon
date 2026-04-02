/*
 * kmscon - Generate Unifont data files
 *
 * Copyright (c) 2012 Ted Kotz <ted@kotz.us>
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

/*
 * Unifont Generator
 * This converts the hex-encoded Unifont data into a C-array that is used by the
 * unifont-font-renderer.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define MAX_DATA_SIZE 255

struct unifont_glyph {
	struct unifont_glyph *next;
	uint32_t codepoint;
	uint8_t len;
	char data[MAX_DATA_SIZE];
};

/*
 * We regroup all glyphs into blocks, of contiguous codepoints, and same width.
 * This allows to better pack the data, and handle some codepoints that are
 * not in the 0xffff range
 */
struct unifont_glyph_block {
	uint32_t codepoint; // First codepoint of the block
	uint32_t offset;    // offset of the data
	uint16_t len;	    // number of glyph in this block
	uint8_t width;	    // glyph width (1 or 2 for double-width glyph)
} __attribute__((__packed__));

static uint8_t hex_val(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	else if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	else if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	fprintf(stderr, "genunifont: invalid hex-code %c\n", c);
	return 0;
}

static int build_unifont_glyph(struct unifont_glyph *g, const char *buf)
{
	int val;
	const char *orig = buf;

	val = 0;
	while (*buf && *buf != ':') {
		val <<= 4;
		val += hex_val(*buf++);
	}

	if (*buf++ != ':') {
		fprintf(stderr, "genunifont: invalid file format: %s\n", orig);
		return -EFAULT;
	}

	g->codepoint = val;
	g->len = 0;
	while (*buf && *buf != '\n' && g->len < MAX_DATA_SIZE) {
		g->data[g->len] = *buf++;
		++g->len;
	}

	return 0;
}

static uint8_t get_width(int len)
{
	if (len == 64)
		return 2;
	if (len == 32)
		return 1;
	fprintf(stderr, "genuifont: invalid length %d\n", len);
	return 0;
}

static void compress_and_write(unsigned char *buf, unsigned long size, FILE *out)
{
	unsigned char *zbuf;
	unsigned long zlen = size;
	uint32_t uncompressed_size;

	zbuf = malloc(zlen);
	if (!zbuf)
		return;

	compress(zbuf, &zlen, buf, size);
	fprintf(stderr, "genuifont: compressed %ld to %ld\n", size, zlen);

	uncompressed_size = size;
	fwrite(&uncompressed_size, sizeof(uncompressed_size), 1, out);
	fwrite(zbuf, zlen, 1, out);
	free(zbuf);
}

static struct unifont_glyph_block *gen_blocks(struct unifont_glyph *list, uint32_t *n_blocks)
{
	struct unifont_glyph *g = list;
	struct unifont_glyph_block *blocks;
	uint32_t i = 0;
	int table_size = 256;
	uint32_t offset = 0;

	blocks = malloc(table_size * sizeof(*blocks));
	if (!blocks) {
		fprintf(stderr, "genunifont: out of memory\n");
		return NULL;
	}

	blocks[i].len = 0;
	blocks[i].offset = 0;
	blocks[i].codepoint = g->codepoint;
	blocks[i].width = get_width(g->len);
	while (g) {
		if (blocks[i].width == get_width(g->len) &&
		    g->codepoint == blocks[i].codepoint + blocks[i].len) {
			/* This glyph can fit in current block */
			blocks[i].len++;
		} else {
			/* Start a new block with this glyph as first glyph */
			offset += blocks[i].len * 16 * blocks[i].width;
			i++;
			if (i >= table_size) {
				table_size *= 2;
				void *tmp = realloc(blocks, table_size * sizeof(*blocks));
				if (!tmp) {
					free(blocks);
					return NULL;
				}
				blocks = tmp;
			}
			blocks[i].len = 1;
			blocks[i].codepoint = g->codepoint;
			blocks[i].width = get_width(g->len);
			blocks[i].offset = offset;
		}
		g = g->next;
	}
	*n_blocks = i + 1;
	return blocks;
}

static void pack_glyph(struct unifont_glyph *list, FILE *out)
{
	struct unifont_glyph *g;
	struct unifont_glyph_block *blocks;
	uint32_t n_blocks = 0;
	uint32_t offset = 0;
	unsigned char *buf;
	uint32_t size;

	blocks = gen_blocks(list, &n_blocks);
	if (!blocks || !n_blocks)
		return;

	size = 4 + n_blocks * sizeof(*blocks);
	for (g = list; g; g = g->next)
		size += g->len / 2;

	buf = malloc(size);

	*(uint32_t *)buf = n_blocks;
	offset += 4;
	memcpy(buf + offset, blocks, sizeof(*blocks) * n_blocks);
	offset += sizeof(*blocks) * n_blocks;
	for (g = list; g; g = g->next) {
		uint8_t val;
		int i;

		for (i = 0; i < g->len; i += 2) {
			val = hex_val(g->data[i]) << 4;
			val |= hex_val(g->data[i + 1]);
			buf[offset++] = val;
		}
	}
	if (offset != size)
		fprintf(stderr, "genunifont: wrong size\n");

	free(blocks);

	compress_and_write(buf, size, out);

	free(buf);
}

static int parse_single_file(FILE *out, FILE *in)
{
	char buf[MAX_DATA_SIZE];
	struct unifont_glyph *g, **iter, *list, *last;
	int ret;
	long status_max, status_cur;
	unsigned long perc_prev, perc_now;

	if (fseek(in, 0, SEEK_END) != 0) {
		fprintf(stderr, "genunifont: cannot seek: %m\n");
		return -EFAULT;
	}

	status_max = ftell(in);
	if (status_max < 0) {
		fprintf(stderr, "genunifont: cannot ftell: %m\n");
		return -EFAULT;
	}

	if (status_max < 1) {
		fprintf(stderr, "genunifont: empty file\n");
		return -EFAULT;
	}

	rewind(in);
	list = NULL;
	last = NULL;
	status_cur = 0;
	perc_prev = 0;
	perc_now = 0;

	fprintf(stderr, "Finished: %3ld%%", perc_now);

	while (fgets(buf, sizeof(buf) - 1, in) != NULL) {
		/* print status update in percent */
		perc_now = status_cur * 100 / status_max;
		if (perc_now > perc_prev) {
			perc_prev = perc_now;
			fprintf(stderr, "\b\b\b\b%3ld%%", perc_now);
			fflush(stderr);
		}
		status_cur += strlen(buf);

		/* ignore comments */
		if (buf[0] == '#')
			continue;

		/* allocate new glyph */
		g = malloc(sizeof(*g));
		if (!g) {
			fprintf(stderr, "genunifont: out of memory\n");
			return -ENOMEM;
		}
		memset(g, 0, sizeof(*g));

		/* read glyph data */
		ret = build_unifont_glyph(g, buf);
		if (ret) {
			free(g);
			return ret;
		}

		/* find glyph position */
		if (last && last->codepoint < g->codepoint) {
			iter = &last->next;
		} else {
			iter = &list;
			while (*iter && (*iter)->codepoint < g->codepoint)
				iter = &(*iter)->next;

			if (*iter && (*iter)->codepoint == g->codepoint) {
				fprintf(stderr, "glyph %d used twice\n", g->codepoint);
				free(g);
				return -EFAULT;
			}
		}

		/* insert glyph into single-linked list */
		g->next = *iter;
		if (!*iter)
			last = g;
		*iter = g;
	}

	fprintf(stderr, "\b\b\b\b%3d%%\n", 100);

	/* pack into table */
	pack_glyph(list, out);

	for (g = list; g;) {
		last = g->next;
		free(g);
		g = last;
	}

	return 0;
}

int main(int argc, char **argv)
{
	FILE *out, *in;
	int ret;

	if (argc < 3) {
		fprintf(stderr, "genunifont: use ./genunifont <outputfile> <inputfiles>\n");
		ret = EXIT_FAILURE;
		goto err_out;
	}

	out = fopen(argv[1], "wb");
	if (!out) {
		fprintf(stderr, "genunifont: cannot open output %s: %m\n", argv[1]);
		ret = EXIT_FAILURE;
		goto err_out;
	}

	in = fopen(argv[2], "rb");
	if (!in) {
		fprintf(stderr, "genunifont: cannot open %s: %m\n", argv[2]);
		ret = EXIT_FAILURE;
	} else {
		ret = parse_single_file(out, in);
		if (ret) {
			fprintf(stderr, "genunifont: parsing input %s failed", argv[2]);
			ret = EXIT_FAILURE;
		} else {
			ret = EXIT_SUCCESS;
		}
		fclose(in);
	}

	fclose(out);
err_out:
	return ret;
}
