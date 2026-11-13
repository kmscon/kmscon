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

#define LOGIND_SERVICE "org.freedesktop.login1"
#define LOGIND_PATH "/org/freedesktop/login1"
#define LOGIND_MANAGER LOGIND_SERVICE ".Manager"
#define LOGIND_SESSION LOGIND_SERVICE ".Session"
#define LOGIND_TIMEOUT 250 // ms

#define LOCALE1_SERVICE "org.freedesktop.locale1"
#define LOCALE1_PATH "/org/freedesktop/locale1"

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
	bool watching;
	dbus_update_xkb_layout_cb xkb_cb;
	void *xkb_data;

	dbus_session_lock_cb lock_cb;
	void *lock_data;
};

static void dbus_idle(struct ev_eloop *eloop, void *unused, void *data);

static bool is_xkb_env_set(void)
{
	return getenv("XKB_DEFAULT_MODEL") || getenv("XKB_DEFAULT_LAYOUT") ||
	       getenv("XKB_DEFAULT_VARIANT") || getenv("XKB_DEFAULT_OPTIONS");
}

struct kmscon_dbus *kmscon_dbus_new(struct ev_eloop *eloop)
{
	struct kmscon_dbus *dbus;
	DBusError error;

	dbus = calloc(1, sizeof(*dbus));
	if (!dbus)
		return NULL;

	dbus->eloop = eloop;

	dbus_error_init(&error);
	dbus->conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (dbus_error_is_set(&error)) {
		log_debug("Connection Error: %s\n", error.message);
		dbus_error_free(&error);
		free(dbus);
		return NULL;
	}
	return dbus;
}

void kmscon_dbus_free(struct kmscon_dbus *dbus)
{
	if (!dbus)
		return;
	ev_eloop_unregister_idle_cb(dbus->eloop, dbus_idle, dbus, EV_SINGLE);
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

/* Parse an a{sv} dict of locale1 properties, the strings point into its message */
static void parse_locale_properties(DBusMessageIter *root_iter, struct rmlvo *rmlvo)
{
	DBusMessageIter dict_iter;
	DBusMessageIter entry_iter;
	DBusMessageIter variant_iter;
	char *property_name;
	char *property_value;

	if (dbus_message_iter_get_arg_type(root_iter) != DBUS_TYPE_ARRAY) {
		log_debug("Not the container type we expect");
		return;
	}

	dbus_message_iter_recurse(root_iter, &dict_iter);
	while (dbus_message_iter_get_arg_type(&dict_iter) == DBUS_TYPE_DICT_ENTRY) {
		dbus_message_iter_recurse(&dict_iter, &entry_iter);
		dbus_message_iter_get_basic(&entry_iter, &property_name);
		dbus_message_iter_next(&entry_iter);

		if (dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_VARIANT) {
			dbus_message_iter_recurse(&entry_iter, &variant_iter);
			if (dbus_message_iter_get_arg_type(&variant_iter) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&variant_iter, &property_value);
				set_rmlvo_property(rmlvo, property_name, property_value);
				log_debug("property %s: %s", property_name, property_value);
			}
		}
		dbus_message_iter_next(&dict_iter);
	}
}

static void set_xkb_env(const char *name, const char *value)
{
	if (value && *value) {
		setenv(name, value, 0);
		log_debug("Set %s to %s", name, value);
	}
}

/* This is called once at startup to set the XKB environment variables from the locale1 properties.
 */
void kmscon_dbus_set_xkb_env_from_locale1(struct kmscon_dbus *dbus)
{
	const char *iface = LOCALE1_SERVICE;
	struct rmlvo rmlvo = {NULL, NULL, NULL, NULL};
	DBusMessage *msg, *reply;
	DBusMessageIter args;
	DBusError error;

	if (is_xkb_env_set())
		return;

	msg = dbus_message_new_method_call(LOCALE1_SERVICE, LOCALE1_PATH, DBUS_INTERFACE_PROPERTIES,
					   "GetAll");
	if (!msg)
		return;
	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID)) {
		dbus_message_unref(msg);
		return;
	}

	dbus_error_init(&error);
	reply = dbus_connection_send_with_reply_and_block(dbus->conn, msg, 2000, &error);
	dbus_message_unref(msg);
	if (!reply) {
		/* This is normal if locale1 is not available on the system */
		if (!dbus_error_has_name(&error, DBUS_ERROR_UNKNOWN_INTERFACE) &&
		    !dbus_error_has_name(&error, DBUS_ERROR_SERVICE_UNKNOWN))
			log_warning("dbus error: %s / %s", error.name, error.message);
		dbus_error_free(&error);
		return;
	}

	if (dbus_message_iter_init(reply, &args))
		parse_locale_properties(&args, &rmlvo);

	set_xkb_env("XKB_DEFAULT_MODEL", rmlvo.model);
	set_xkb_env("XKB_DEFAULT_LAYOUT", rmlvo.layout);
	set_xkb_env("XKB_DEFAULT_VARIANT", rmlvo.variant);
	set_xkb_env("XKB_DEFAULT_OPTIONS", rmlvo.options);
	dbus_message_unref(reply);
}

/*
 * logind
 * Sessions are found by their leader, the login process kmscon started on the
 * terminal. The calls block for at most LOGIND_TIMEOUT.
 */

static int logind_error(DBusError *err)
{
	int ret = -EIO;

	if (dbus_error_has_name(err, LOGIND_SERVICE ".NoSessionForPID") ||
	    dbus_error_has_name(err, LOGIND_SERVICE ".NoSuchSession") ||
	    dbus_error_has_name(err, DBUS_ERROR_UNKNOWN_OBJECT))
		ret = -ENOENT;
	else if (dbus_error_has_name(err, DBUS_ERROR_SERVICE_UNKNOWN) ||
		 dbus_error_has_name(err, DBUS_ERROR_NAME_HAS_NO_OWNER))
		ret = -ENOTCONN;
	else
		log_debug("logind error: %s: %s", err->name, err->message);

	dbus_error_free(err);
	return ret;
}

/* Takes the reference on @msg */
static int logind_call(struct kmscon_dbus *dbus, DBusMessage *msg, DBusMessage **reply)
{
	DBusError err;

	dbus_error_init(&err);
	*reply = dbus_connection_send_with_reply_and_block(dbus->conn, msg, LOGIND_TIMEOUT, &err);
	dbus_message_unref(msg);
	if (!*reply)
		return logind_error(&err);
	return 0;
}

static int get_session_by_pid(struct kmscon_dbus *dbus, pid_t pid, char **path)
{
	DBusMessage *msg, *reply;
	DBusError err;
	uint32_t upid = pid;
	const char *p;
	int ret;

	msg = dbus_message_new_method_call(LOGIND_SERVICE, LOGIND_PATH, LOGIND_MANAGER,
					   "GetSessionByPID");
	if (!msg)
		return -ENOMEM;
	if (!dbus_message_append_args(msg, DBUS_TYPE_UINT32, &upid, DBUS_TYPE_INVALID)) {
		dbus_message_unref(msg);
		return -ENOMEM;
	}

	ret = logind_call(dbus, msg, &reply);
	if (ret)
		return ret;

	dbus_error_init(&err);
	if (!dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID)) {
		dbus_message_unref(reply);
		return logind_error(&err);
	}
	*path = strdup(p);
	dbus_message_unref(reply);
	return *path ? 0 : -ENOMEM;
}

/* Read a numeric session property: Leader (u), or the uid of User ((uo)) */
static int get_session_uint(struct kmscon_dbus *dbus, const char *path, const char *name,
			    uint32_t *val)
{
	const char *iface = LOGIND_SESSION;
	DBusMessageIter args, variant, field;
	DBusMessage *msg, *reply;
	int ret;

	msg = dbus_message_new_method_call(LOGIND_SERVICE, path, DBUS_INTERFACE_PROPERTIES, "Get");
	if (!msg)
		return -ENOMEM;
	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name,
				      DBUS_TYPE_INVALID)) {
		dbus_message_unref(msg);
		return -ENOMEM;
	}

	ret = logind_call(dbus, msg, &reply);
	if (ret)
		return ret;

	ret = -EINVAL;
	if (dbus_message_iter_init(reply, &args) &&
	    dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&args, &variant);
		if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRUCT) {
			dbus_message_iter_recurse(&variant, &field);
			variant = field;
		}
		if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32) {
			dbus_message_iter_get_basic(&variant, val);
			ret = 0;
		}
	}
	dbus_message_unref(reply);
	return ret;
}

/* A login prompt belongs to the session of an ancestor, which doesn't count */
static int get_led_session(struct kmscon_dbus *dbus, pid_t leader, char **path)
{
	uint32_t val;
	int ret;

	ret = get_session_by_pid(dbus, leader, path);
	if (ret)
		return ret;

	ret = get_session_uint(dbus, *path, "Leader", &val);
	if (!ret && val != (uint32_t)leader)
		ret = -ENOENT;
	if (ret) {
		free(*path);
		*path = NULL;
	}
	return ret;
}

/* The owner of the logind session led by @leader. Returns -ENOENT if there is
 * none, and -ENOTCONN if logind is not available. */
int kmscon_dbus_get_session_owner(struct kmscon_dbus *dbus, pid_t leader, uid_t *uid)
{
	uint32_t val;
	char *path;
	int ret;

	if (!dbus)
		return -ENOTCONN;
	if (leader <= 0 || !uid)
		return -EINVAL;

	ret = get_led_session(dbus, leader, &path);
	if (ret)
		return ret;

	ret = get_session_uint(dbus, path, "User", &val);
	if (!ret)
		*uid = val;
	free(path);
	return ret;
}

void kmscon_dbus_set_locked_hint(struct kmscon_dbus *dbus, pid_t leader, bool locked)
{
	dbus_bool_t value = locked;
	DBusMessage *msg;
	char *path;

	if (!dbus || leader <= 0 || get_led_session(dbus, leader, &path))
		return;

	msg = dbus_message_new_method_call(LOGIND_SERVICE, path, LOGIND_SESSION, "SetLockedHint");
	free(path);
	if (!msg)
		return;

	dbus_message_set_no_reply(msg, TRUE);
	if (!dbus_message_append_args(msg, DBUS_TYPE_BOOLEAN, &value, DBUS_TYPE_INVALID) ||
	    !dbus_connection_send(dbus->conn, msg, NULL))
		log_warning("cannot send LockedHint to logind");
	else
		dbus_connection_flush(dbus->conn);
	dbus_message_unref(msg);
}

/* The unique bus name of the connection that owns @service */
static int get_name_owner(struct kmscon_dbus *dbus, const char *service, char **owner)
{
	DBusMessage *msg, *reply;
	DBusError err;
	const char *name;
	int ret;

	msg = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
					   "GetNameOwner");
	if (!msg)
		return -ENOMEM;
	if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID)) {
		dbus_message_unref(msg);
		return -ENOMEM;
	}

	ret = logind_call(dbus, msg, &reply);
	if (ret)
		return ret;

	dbus_error_init(&err);
	if (!dbus_message_get_args(reply, &err, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
		dbus_message_unref(reply);
		return logind_error(&err);
	}
	*owner = strdup(name);
	dbus_message_unref(reply);
	return *owner ? 0 : -ENOMEM;
}

/* Any client can send a signal to us directly, bypassing the sender of the
 * match rules, so only trust the signals sent by the owner of @service */
static bool is_sent_by(struct kmscon_dbus *dbus, DBusMessage *msg, const char *service)
{
	const char *sender = dbus_message_get_sender(msg);
	char *owner;
	bool ret;

	if (!sender)
		return false;
	if (get_name_owner(dbus, service, &owner)) {
		log_warning("cannot find %s on the bus, ignoring its signals", service);
		return false;
	}

	ret = !strcmp(sender, owner);
	if (!ret)
		log_warning("ignoring %s signal not sent by %s (sender %s)",
			    dbus_message_get_member(msg), service, sender);
	free(owner);
	return ret;
}

static void handle_locale1(struct kmscon_dbus *dbus, DBusMessage *msg)
{
	struct rmlvo rmlvo = {NULL, NULL, NULL, NULL};
	DBusMessageIter args;
	const char *iface;

	if (!dbus->xkb_cb || !dbus_message_has_path(msg, LOCALE1_PATH))
		return;

	if (!dbus_message_iter_init(msg, &args) ||
	    dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
		return;
	dbus_message_iter_get_basic(&args, &iface);
	if (strcmp(iface, LOCALE1_SERVICE) || !dbus_message_iter_next(&args))
		return;

	/* Other properties, like the Locale, don't change the keymap */
	parse_locale_properties(&args, &rmlvo);
	if (!rmlvo.model && !rmlvo.layout && !rmlvo.variant && !rmlvo.options)
		return;

	if (is_sent_by(dbus, msg, LOCALE1_SERVICE))
		dbus->xkb_cb(rmlvo.model, rmlvo.layout, rmlvo.variant, rmlvo.options,
			     dbus->xkb_data);
}

static void handle_lock(struct kmscon_dbus *dbus, DBusMessage *msg, bool lock)
{
	const char *path = dbus_message_get_path(msg);
	uint32_t leader;

	if (!dbus->lock_cb || !path || !is_sent_by(dbus, msg, LOGIND_SERVICE))
		return;

	/* logind signals every session: those of other programs are not ours */
	if (get_session_uint(dbus, path, "Leader", &leader))
		return;

	log_debug("logind %s request for session %s (leader %u)", lock ? "lock" : "unlock", path,
		  leader);
	dbus->lock_cb(leader, lock, dbus->lock_data);
}

/* The messages are handled here, as the libdbus callbacks must neither block
 * nor dispatch. No libdbus handler is registered, so pop them instead of
 * calling dbus_connection_dispatch(), which would drop them. */
static void dbus_idle(struct ev_eloop *eloop, void *unused, void *data)
{
	struct kmscon_dbus *dbus = data;
	DBusMessage *msg;

	/* Also gets the messages read while waiting for a reply */
	while ((msg = dbus_connection_pop_message(dbus->conn))) {
		log_debug("received message %s.%s", dbus_message_get_interface(msg),
			  dbus_message_get_member(msg));
		if (dbus_message_is_signal(msg, DBUS_INTERFACE_PROPERTIES, "PropertiesChanged"))
			handle_locale1(dbus, msg);
		else if (dbus_message_is_signal(msg, LOGIND_SESSION, "Lock"))
			handle_lock(dbus, msg, true);
		else if (dbus_message_is_signal(msg, LOGIND_SESSION, "Unlock"))
			handle_lock(dbus, msg, false);
		dbus_message_unref(msg);
	}
}

static void schedule_idle(struct kmscon_dbus *dbus)
{
	int ret;

	ret = ev_eloop_register_idle_cb(dbus->eloop, dbus_idle, dbus, EV_ONESHOT | EV_SINGLE);
	if (ret)
		log_error("cannot schedule dbus idle callback: %d", ret);
}

static void schedule_dispatch(struct kmscon_dbus *dbus)
{
	if (dbus_connection_get_dispatch_status(dbus->conn) == DBUS_DISPATCH_DATA_REMAINS)
		schedule_idle(dbus);
}

/* Also called when messages are read while waiting for a reply, which doesn't
 * wake the socket up */
static void handle_dispatch_status(DBusConnection *conn, DBusDispatchStatus status, void *data)
{
	if (status == DBUS_DISPATCH_DATA_REMAINS)
		schedule_idle(data);
}

static void dbus_watch_event(struct ev_fd *fd, int mask, void *data)
{
	struct DBusWatch *watch = data;
	struct kmscon_dbus *dbus = dbus_watch_get_data(watch);

	unsigned int flags = 0;

	log_debug("dbus message received\n");

	if (mask & EV_READABLE)
		flags |= DBUS_WATCH_READABLE;
	if (mask & EV_WRITEABLE)
		flags |= DBUS_WATCH_WRITABLE;

	dbus_watch_handle(watch, flags);

	schedule_dispatch(dbus);
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

	ret = ev_eloop_new_fd(dbus->eloop, &dbus->watch_fd, fd, ev_flags, dbus_watch_event, watch);

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

/* Shared by the locale1 and logind listeners */
static void setup_watch(struct kmscon_dbus *dbus)
{
	if (dbus->watching)
		return;

	dbus_connection_set_watch_functions(dbus->conn, dbus_add_watch, dbus_remove_watch,
					    dbus_toggle_watch, dbus, NULL);
	dbus_connection_set_dispatch_status_function(dbus->conn, handle_dispatch_status, dbus,
						     NULL);
	dbus->watching = true;

	/* The status function only reports changes */
	schedule_dispatch(dbus);
}

/* Listen to locale1 properties changed, and update the XKB layout if needed */
int kmscon_dbus_listen_locale1(struct kmscon_dbus *dbus, dbus_update_xkb_layout_cb cb, void *data)
{
	DBusError error;

	log_debug("Listening to locale1\n");

	if (!cb)
		return -EINVAL;

	dbus->xkb_cb = cb;
	dbus->xkb_data = data;

	dbus_error_init(&error);
	setup_watch(dbus);

	const char *match_rule = "type='signal',"
				 "sender='" LOCALE1_SERVICE "',"
				 "path='" LOCALE1_PATH "',"
				 "interface='" DBUS_INTERFACE_PROPERTIES "',"
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

/* Listen to logind Lock and Unlock signals */
int kmscon_dbus_listen_logind(struct kmscon_dbus *dbus, dbus_session_lock_cb cb, void *data)
{
	const char *match_rule = "type='signal',"
				 "sender='" LOGIND_SERVICE "',"
				 "interface='" LOGIND_SESSION "'";
	DBusError error;

	if (!dbus || !cb)
		return -EINVAL;

	dbus->lock_cb = cb;
	dbus->lock_data = data;
	setup_watch(dbus);

	dbus_error_init(&error);
	dbus_bus_add_match(dbus->conn, match_rule, &error);
	if (dbus_error_is_set(&error)) {
		log_warning("cannot listen to logind: %s", error.message);
		dbus_error_free(&error);
		return -EIO;
	}
	dbus_connection_flush(dbus->conn);

	log_info("Listening to logind lock requests");
	return 0;
}
