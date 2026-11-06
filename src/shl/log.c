/*
 * Log/Debug Interface
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Dedicated to the Public Domain
 */

/*
 * Log/Debug API Implementation
 * We provide thread-safety so we need a global lock. Function which
 * are prefixed with log__* need the lock to be held. All other functions must
 * be called without the lock held.
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "githead.h"
#include "log.h"
#include "misc.h"

/*
 * Locking
 * We need a global locking mechanism. Use pthread here.
 */

static pthread_mutex_t log__mutex = PTHREAD_MUTEX_INITIALIZER;

static inline void log_lock()
{
	pthread_mutex_lock(&log__mutex);
}

static inline void log_unlock()
{
	pthread_mutex_unlock(&log__mutex);
}

const char *LOG_SUBSYSTEM = NULL;

/*
 * By default, the log level is set to LOG_NOTICE, so LOG_DEBUG and LOG_INFO
 * messages are disabled.
 */
static enum log_severity log_level = LOG_NOTICE;

void log_set_level(enum log_severity level)
{
	log_level = level;
}

#define LOG_SUBSYSTEM "log"

static const char *prefix[LOG_SEV_NUM] = {
	"Fatal", "Alert", "Critical", "Error", "Warning", "Notice", "Info", "Debug",
};

/*
 * Basic logger
 * The log__submit function writes the message into the current log-target. It
 * must be called with log__mutex locked.
 * log__format does the same but first converts the argument list into a
 * va_list.
 * By default the current time elapsed since the first message was logged is
 * prepended to the message. file, line and func information are appended to the
 * message if sev == LOG_DEBUG.
 * The subsystem, if not NULL, is prepended as "SUBS: " to the message and a
 * newline is always appended by default. Multiline-messages are not allowed and
 * do not make sense here.
 */

static void log__submit(enum log_severity level, const char *subs, const char *format, va_list args)
{
	FILE *out = stderr;
	bool nl;
	size_t len;

	if (level > log_level)
		return;

	// Print severity prefix if level is LOG_WARNING or lower
	if (level <= LOG_WARNING)
		fprintf(out, "%s: ", prefix[level]);

	if (subs)
		fprintf(out, "%s: ", subs);

	len = strlen(format);
	nl = len > 0 && format[len - 1] == '\n';

	vfprintf(out, format, args);

	if (!nl)
		fprintf(out, "\n");
}

SHL_EXPORT
void log_submit(enum log_severity level, const char *subs, const char *format, va_list args)
{
	int saved_errno = errno;

	log_lock();
	log__submit(level, subs, format, args);
	log_unlock();

	errno = saved_errno;
}

SHL_EXPORT
void log_format(enum log_severity level, const char *subs, const char *format, ...)
{
	va_list list;
	int saved_errno = errno;

	va_start(list, format);
	log_lock();
	log__submit(level, subs, format, list);
	log_unlock();
	va_end(list);

	errno = saved_errno;
}

void log_print_init(const char *appname)
{
	if (!appname)
		appname = "<unknown>";
	log_format(LOG_NOTICE, "log", "%s Revision %s %s %s\n", appname, shl_git_head, __DATE__,
		   __TIME__);
}
