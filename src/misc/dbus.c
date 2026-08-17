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
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "dbus.h"
#include "shl/eloop.h"
#include "shl/log.h"

#define LOG_SUBSYSTEM "dbus"

struct rmlvo {
	const char *model;
	const char *layout;
	const char *variant;
	const char *options;
};

struct kmscon_dbus {
	DBusConnection *conn;
	struct ev_eloop *eloop;
	struct ev_fd *watch_fd;
	dbus_update_xkb_layout_cb cb;
	void *data;
};

static void set_env_from_locale1_properties(DBusConnection *conn, const char *property_name,
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
	    dbus_error_has_name(&error, DBUS_ERROR_UNKNOWN_PROPERTY) ||
	    dbus_error_has_name(&error, DBUS_ERROR_SERVICE_UNKNOWN)) {
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

/* This is called once at startup to set the XKB environment variables from the locale1 properties.
 */
void kmscon_dbus_set_xkb_env_from_locale1(struct kmscon_dbus *dbus)
{
	DBusError error;

	if (is_xkb_env_set())
		return;

	dbus_error_init(&error);

	set_env_from_locale1_properties(dbus->conn, "X11Model", "XKB_DEFAULT_MODEL");
	set_env_from_locale1_properties(dbus->conn, "X11Layout", "XKB_DEFAULT_LAYOUT");
	set_env_from_locale1_properties(dbus->conn, "X11Variant", "XKB_DEFAULT_VARIANT");
	set_env_from_locale1_properties(dbus->conn, "X11Options", "XKB_DEFAULT_OPTIONS");
}

struct kmscon_dbus *kmscon_dbus_new(struct ev_eloop *eloop)
{
	struct kmscon_dbus *dbus;
	DBusError error;

	dbus = malloc(sizeof(*dbus));
	if (!dbus)
		return NULL;
	memset(dbus, 0, sizeof(*dbus));

	dbus->eloop = eloop;

	dbus_error_init(&error);
	dbus->conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (dbus_error_is_set(&error)) {
		log_debug("Connection Error: %s\n", error.message);
		dbus_error_free(&error);
		return NULL;
	}
	return dbus;
}

void kmscon_dbus_free(struct kmscon_dbus *dbus)
{
	if (!dbus)
		return;
	if (dbus->watch_fd) {
		ev_eloop_rm_fd(dbus->watch_fd);
		dbus->watch_fd = NULL;
	}
	dbus_connection_unref(dbus->conn);
	free(dbus);
}

static void set_rmlvo_property(struct rmlvo *rmlvo, const char *property_name,
			       const char *property_value)
{
	if (!strcmp(property_name, "X11Model")) {
		rmlvo->model = property_value;
	} else if (!strcmp(property_name, "X11Layout")) {
		rmlvo->layout = property_value;
	} else if (!strcmp(property_name, "X11Variant")) {
		rmlvo->variant = property_value;
	} else if (!strcmp(property_name, "X11Options")) {
		rmlvo->options = property_value;
	}
}

/* Parse the locale1 properties changed message and update the rmlvo struct */
void parse_locale_properties(DBusMessage *msg, struct rmlvo *rmlvo)
{
	DBusMessageIter root_iter;
	DBusMessageIter dict_iter;
	DBusMessageIter entry_iter;
	DBusMessageIter variant_iter;
	char *property_name;
	char *property_value;

	if (!dbus_message_iter_init(msg, &root_iter)) {
		log_debug("Message has no arguments.\n");
		return;
	}

	dbus_message_iter_next(&root_iter);
	if (dbus_message_iter_get_arg_type(&root_iter) != DBUS_TYPE_ARRAY) {
		log_debug("Not the container type we expect\n");
		return;
	}

	dbus_message_iter_recurse(&root_iter, &dict_iter);
	while (dbus_message_iter_get_arg_type(&dict_iter) == DBUS_TYPE_DICT_ENTRY) {

		dbus_message_iter_recurse(&dict_iter, &entry_iter);

		dbus_message_iter_get_basic(&entry_iter, &property_name);

		dbus_message_iter_next(&entry_iter);

		if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_VARIANT) {

			dbus_message_iter_recurse(&entry_iter, &variant_iter);

			if (dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&variant_iter, &property_value);
				set_rmlvo_property(rmlvo, property_name, property_value);
				log_debug("property %s: %s\n", property_name, property_value);
			}
		}
		dbus_message_iter_next(&dict_iter);
	}
}

static void handle_dispatch_status(DBusConnection *conn, DBusDispatchStatus status, void *data)
{
	struct kmscon_dbus *dbus = data;
	struct rmlvo rmlvo = {NULL, NULL, NULL, NULL};

	if (status == DBUS_DISPATCH_DATA_REMAINS) {
		DBusMessage *msg = dbus_connection_pop_message(dbus->conn);
		if (msg) {
			log_debug("Received message %s\n", dbus_message_get_signature(msg));
			if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties",
						   "PropertiesChanged")) {
				parse_locale_properties(msg, &rmlvo);
				if (dbus->cb)
					dbus->cb(rmlvo.model, rmlvo.layout, rmlvo.variant,
						 rmlvo.options, dbus->data);
			}
			dbus_message_unref(msg);
		}
	}
}

static void locale1_properties_changed(struct ev_fd *fd, int mask, void *data)
{
	struct DBusWatch *watch = data;
	struct kmscon_dbus *dbus = dbus_watch_get_data(watch);

	unsigned int flags = 0;

	log_debug("locale1 property changed\n");

	if (mask & EV_READABLE)
		flags |= DBUS_WATCH_READABLE;
	if (mask & EV_WRITEABLE)
		flags |= DBUS_WATCH_WRITABLE;

	dbus_watch_handle(watch, flags);

	while (dbus_connection_get_dispatch_status(dbus->conn) == DBUS_DISPATCH_DATA_REMAINS) {
		dbus_connection_dispatch(dbus->conn);
		// DBUS-1 API is a pain, but at least that works for this simple use case
		handle_dispatch_status(dbus->conn, DBUS_DISPATCH_DATA_REMAINS, dbus);
	}
}

/* Only register one read watch, as we're not interested in writes */
static unsigned int dbus_add_watch(DBusWatch *watch, void *data)
{
	struct kmscon_dbus *dbus = data;
	int fd = dbus_watch_get_unix_fd(watch);
	unsigned int flags = dbus_watch_get_flags(watch);
	unsigned int ev_flags = 0;
	int ret = 0;

	log_debug("Adding watch for fd %d with flags %d %p %d\n", fd, flags, watch,
		  dbus_watch_get_enabled(watch));

	if (!dbus_watch_get_enabled(watch))
		return TRUE;

	if (dbus->watch_fd) {
		log_warning("dbus watch already registered");
		return TRUE;
	}
	if (flags & DBUS_WATCH_WRITABLE)
		ev_flags |= EV_WRITEABLE;
	if (flags & DBUS_WATCH_READABLE)
		ev_flags |= EV_READABLE;

	ret = ev_eloop_new_fd(dbus->eloop, &dbus->watch_fd, fd, ev_flags,
			      locale1_properties_changed, watch);

	dbus_watch_set_data(watch, dbus, NULL);

	if (ret)
		log_error("cannot add watch for fd %d: %d %d %p", fd, ret, ev_flags, watch);
	return TRUE;
}

static void dbus_remove_watch(DBusWatch *watch, void *data)
{
	struct kmscon_dbus *dbus = data;
	log_debug("Removing watch for fd %d\n", dbus_watch_get_unix_fd(watch));

	if (dbus_watch_get_data(watch) != dbus)
		return;

	ev_eloop_rm_fd(dbus->watch_fd);
	dbus->watch_fd = NULL;
	dbus_watch_set_data(watch, NULL, NULL);
	return;
}

static void dbus_toggle_watch(DBusWatch *watch, void *data)
{
	log_debug("Toggling watch for fd %d\n", dbus_watch_get_unix_fd(watch));

	if (dbus_watch_get_enabled(watch))
		dbus_add_watch(watch, data);
	else
		dbus_remove_watch(watch, data);
}

/* Listen to locale1 properties changed, and update the XKB layout if needed */
int kmscon_dbus_listen_locale1(struct kmscon_dbus *dbus, dbus_update_xkb_layout_cb cb, void *data)
{
	DBusError error;

	log_debug("Listening to locale1\n");

	if (!cb)
		return -EINVAL;

	dbus->cb = cb;
	dbus->data = data;

	dbus_error_init(&error);

	dbus_connection_set_watch_functions(dbus->conn, dbus_add_watch, dbus_remove_watch,
					    dbus_toggle_watch, dbus, NULL);

	dbus_connection_set_dispatch_status_function(dbus->conn, handle_dispatch_status, dbus,
						     NULL);

	const char *match_rule = "type='signal',"
				 "sender='org.freedesktop.locale1',"
				 "path='/org/freedesktop/locale1',"
				 "interface='org.freedesktop.DBus.Properties',"
				 "member='PropertiesChanged'";

	dbus_bus_add_match(dbus->conn, match_rule, &error);

	if (dbus_error_is_set(&error)) {
		log_debug("Error adding match: %s\n", error.message);
		dbus_error_free(&error);
		return -EIO;
	}
	dbus_connection_flush(dbus->conn);

	log_info("Listening to locale1 changes");

	return 0;
}
