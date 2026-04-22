/*
 * shl - Simple LRU Cache
 *
 * Copyright (c) 2026 Red Hat.
 * Author: Jocelyn Falempe <jfalempe@redhat.com>
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

#include <errno.h>
#include <string.h>
#include "htable.h"
#include "shl_dlist.h"
#include "shl_log.h"

struct shl_lru {
	unsigned int max_size;
	unsigned int size;
	struct htable tbl;
	struct shl_dlist list;
};

struct shl_lru_entry {
	struct shl_dlist list;
	uint64_t key;
	void *value;
};

static size_t shl_lru_rehash(const void *ele, void *priv)
{
	const struct shl_lru_entry *ent = ele;

	return ent->key;
}

static inline struct shl_lru *shl_lru_new(unsigned int max_size)
{
	struct shl_lru *lru;

	if (max_size == 0)
		return NULL;

	lru = malloc(sizeof(*lru));
	if (!lru)
		return NULL;
	memset(lru, 0, sizeof(*lru));

	lru->max_size = max_size;
	lru->size = 0;
	shl_dlist_init(&lru->list);
	htable_init(&lru->tbl, shl_lru_rehash, lru);

	return lru;
}

static inline void shl_lru_free(struct shl_lru *lru)
{
	struct htable_iter i;
	struct shl_lru_entry *entry;

	if (!lru)
		return;

	for (entry = htable_first(&lru->tbl, &i); entry; entry = htable_next(&lru->tbl, &i)) {
		htable_delval(&lru->tbl, &i);
		free(entry->value);
		free(entry);
	}

	htable_clear(&lru->tbl);
	free(lru);
}

static inline void *shl_lru_get(struct shl_lru *lru, uint64_t key)
{
	struct htable_iter i;
	struct shl_lru_entry *entry;

	if (!lru)
		return NULL;

	for (entry = htable_firstval(&lru->tbl, &i, key); entry;
	     entry = htable_nextval(&lru->tbl, &i, key)) {
		if (key == entry->key) {
			/* Put this entry at the top of the list */
			shl_dlist_unlink(&entry->list);
			shl_dlist_link(&lru->list, &entry->list);
			return entry->value;
		}
	}
	return NULL;
}

static inline int shl_lru_evict(struct shl_lru *lru)
{
	struct shl_lru_entry *last, *entry;
	struct htable_iter i;

	last = shl_dlist_last(&lru->list, struct shl_lru_entry, list);

	for (entry = htable_firstval(&lru->tbl, &i, last->key); entry;
	     entry = htable_nextval(&lru->tbl, &i, last->key)) {
		if (last == entry) {
			htable_delval(&lru->tbl, &i);
			shl_dlist_unlink(&last->list);
			free(entry->value);
			free(entry);
			lru->size--;
			return 0;
		}
	}
	log_warning("LRU evict failed key not found %" PRIu64, last->key);
	return -ENOENT;
}

static inline int shl_lru_insert(struct shl_lru *lru, uint64_t key, void *value)
{
	struct shl_lru_entry *entry;

	if (!lru)
		return -EINVAL;

	if (lru->size >= lru->max_size && shl_lru_evict(lru) < 0)
		return -ENOMEM;

	entry = malloc(sizeof(*entry));
	if (!entry)
		return -ENOMEM;
	entry->key = key;
	entry->value = value;

	if (!htable_add(&lru->tbl, key, entry)) {
		free(entry);
		return -ENOMEM;
	}
	shl_dlist_link(&lru->list, &entry->list);
	lru->size++;
	return 0;
}
