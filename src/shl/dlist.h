/*
 * shl - Double Linked List
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
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
 * A simple double linked list implementation
 */

#ifndef SHL_DLIST_H
#define SHL_DLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* miscellaneous */

#define shl_offsetof(pointer, type, member)                                                        \
	({                                                                                         \
		const typeof(((type *)0)->member) *__ptr = (pointer);                              \
		(type *)(((char *)__ptr) - offsetof(type, member));                                \
	})

/* double linked list */

struct dlist {
	struct dlist *next;
	struct dlist *prev;
};

#define SHL_DLIST_INIT(head) {&(head), &(head)}

static inline void dlist_init(struct dlist *list)
{
	list->next = list;
	list->prev = list;
}

static inline void dlist__link(struct dlist *prev, struct dlist *next, struct dlist *n)
{
	next->prev = n;
	n->next = next;
	n->prev = prev;
	prev->next = n;
}

static inline void dlist_link(struct dlist *head, struct dlist *n)
{
	return dlist__link(head, head->next, n);
}

static inline void dlist_link_tail(struct dlist *head, struct dlist *n)
{
	return dlist__link(head->prev, head, n);
}

static inline void dlist__unlink(struct dlist *prev, struct dlist *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void dlist_unlink(struct dlist *e)
{
	dlist__unlink(e->prev, e->next);
	e->prev = NULL;
	e->next = NULL;
}

static inline bool dlist_empty(struct dlist *head)
{
	return head->next == head;
}

#define dlist_entry(ptr, type, member) shl_offsetof((ptr), type, member)

#define dlist_first(head, type, member)                                                            \
	(dlist_empty(head) ? NULL : dlist_entry((head)->next, type, member))

#define dlist_last(head, type, member)                                                             \
	(dlist_empty(head) ? NULL : dlist_entry((head)->prev, type, member))

#define dlist_next(iter, head, member)                                                             \
	((iter)->member.next == (head) ? NULL                                                      \
				       : dlist_entry((iter)->member.next, typeof(*iter), list))

#define dlist_prev(iter, head, member)                                                             \
	((iter)->member.prev == (head) ? NULL                                                      \
				       : dlist_entry((iter)->member.prev, typeof(*iter), list))

#define dlist_for_each_entry(iter, head, member)                                                   \
	for (iter = dlist_entry((head)->next, typeof(*iter), member); &iter->member != (head);     \
	     iter = dlist_entry(iter->member.next, typeof(*iter), member))

#define dlist_for_each_entry_safe(iter, tmp, head, member)                                         \
	for (iter = dlist_entry((head)->next, typeof(*iter), member), tmp = iter->member.next;     \
	     &iter->member != (head);                                                              \
	     iter = dlist_entry(tmp, typeof(*iter), member), tmp = iter->member.next)

#define dlist_for_each_entry_reverse(iter, head, member)                                           \
	for (iter = dlist_entry((head)->prev, typeof(*iter), member); &iter->member != (head);     \
	     iter = dlist_entry(iter->member.prev, typeof(*iter), member))

#define dlist_for_each_entry_reverse_safe(iter, tmp, head, member)                                 \
	for (iter = dlist_entry((head)->prev, typeof(*iter), member), tmp = iter->member.prev;     \
	     &iter->member != (head);                                                              \
	     iter = dlist_entry(tmp, typeof(*iter), member), tmp = iter->member.prev)

#endif /* SHL_DLIST_H */
