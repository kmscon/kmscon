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

#include <dbus/dbus.h>
#include <stdlib.h>
#include <string.h>

#include "shl/log.h"

#define LOG_SUBSYSTEM "dbus"

static void dbus_set_locale_property(DBusConnection *conn, const char *property_name,
				     const char *env_name)
{
	DBusError error;
	DBusMessage *msg;
	DBusMessage *reply;
	DBusMessageIter args, variant_iter;
	const char *interface_name = "org.freedesktop.locale1";
	char *value = NULL;

	dbus_error_init(&error);

	msg = dbus_message_new_method_call("org.freedesktop.locale1", "/org/freedesktop/locale1",
					   "org.freedesktop.DBus.Properties", "Get");
	if (!msg)
		return;

	dbus_message_iter_init_append(msg, &args);
	if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &interface_name) ||
	    !dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &property_name)) {
		dbus_message_unref(msg);
		return;
	}

	reply = dbus_connection_send_with_reply_and_block(conn, msg, 2000, &error);
	dbus_message_unref(msg);

	if (dbus_error_has_name(&error, DBUS_ERROR_UNKNOWN_INTERFACE) ||
	    dbus_error_has_name(&error, DBUS_ERROR_UNKNOWN_PROPERTY)) {
		/* This is normal if the interface is not supported by the system */
		dbus_error_free(&error);
		return;
	} else if (dbus_error_is_set(&error)) {
		log_warning("dbus error: %s / %s", error.name, error.message);
		dbus_error_free(&error);
		return;
	} else if (!reply) {
		log_warning("no reply from dbus");
		return;
	}

	if (!dbus_message_iter_init(reply, &args)) {
		log_debug("Reply message has no arguments.\n");
	} else if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_VARIANT) {
		log_debug("Reply argument is not a variant.\n");
	} else {
		dbus_message_iter_recurse(&args, &variant_iter);

		if (dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&variant_iter, &value);
			if (strlen(value) > 0) {
				setenv(env_name, value, 0);
				log_debug("Set %s to %s\n", env_name, value);
			}
		} else {
			log_debug("Unexpected inner data type in variant.\n");
		}
	}

	dbus_message_unref(reply);
	return;
}

static bool is_xkb_env_set(void)
{
	return getenv("XKB_DEFAULT_MODEL") || getenv("XKB_DEFAULT_LAYOUT") ||
	       getenv("XKB_DEFAULT_VARIANT") || getenv("XKB_DEFAULT_OPTIONS");
}

void dbus_set_xkb_env_from_locale(void)
{
	DBusError error;
	DBusConnection *conn;

	if (is_xkb_env_set())
		return;

	dbus_error_init(&error);

	conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (dbus_error_is_set(&error)) {
		log_debug("Connection Error: %s\n", error.message);
		dbus_error_free(&error);
		return;
	}
	if (!conn)
		return;

	dbus_set_locale_property(conn, "X11Model", "XKB_DEFAULT_MODEL");
	dbus_set_locale_property(conn, "X11Layout", "XKB_DEFAULT_LAYOUT");
	dbus_set_locale_property(conn, "X11Variant", "XKB_DEFAULT_VARIANT");
	dbus_set_locale_property(conn, "X11Options", "XKB_DEFAULT_OPTIONS");

	dbus_connection_unref(conn);
}
