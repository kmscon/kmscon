/*
 * dbus - D-Bus support
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

#ifndef _KMSCON_DBUS_H
#define _KMSCON_DBUS_H

struct ev_eloop;
struct kmscon_dbus;

typedef void (*dbus_update_xkb_layout_cb)(const char *model, const char *layout,
					  const char *variant, const char *options, void *data);

#ifdef BUILD_ENABLE_DBUS
void kmscon_dbus_set_xkb_env_from_locale1(struct kmscon_dbus *dbus);
struct kmscon_dbus *kmscon_dbus_new(struct ev_eloop *eloop);
void kmscon_dbus_free(struct kmscon_dbus *dbus);
int kmscon_dbus_listen_locale1(struct kmscon_dbus *dbus, dbus_update_xkb_layout_cb cb, void *data);

#else
static inline void kmscon_dbus_set_xkb_env_from_locale1(struct kmscon_dbus *dbus)
{
	return;
}
static inline struct kmscon_dbus *kmscon_dbus_new(struct ev_eloop *eloop)
{
	return NULL;
}
static inline void kmscon_dbus_free(struct kmscon_dbus *dbus)
{
	return;
}
static inline int kmscon_dbus_listen_locale1(struct kmscon_dbus *dbus, dbus_update_xkb_layout_cb cb,
					     void *data)
{
	return 0;
}
#endif /* BUILD_ENABLE_DBUS */
#endif /* _KMSCON_DBUS_H */