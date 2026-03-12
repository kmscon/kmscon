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
#include "shl_githead.h"
#include "shl_log.h"
#include "shl_misc.h"

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

/*
 * Time Management
 * We print seconds and microseconds since application start for each
 * log-message.
 */

static struct timeval log__ftime;

static void log__time(long long *sec, long long *usec)
{
	struct timeval t;

	if (log__ftime.tv_sec == 0 && log__ftime.tv_usec == 0) {
		gettimeofday(&log__ftime, NULL);
		*sec = 0;
		*usec = 0;
	} else {
		gettimeofday(&t, NULL);
		*sec = t.tv_sec - log__ftime.tv_sec;
		*usec = (long long)t.tv_usec - (long long)log__ftime.tv_usec;
		if (*usec < 0) {
			*sec -= 1;
			*usec = 1000000 + *usec;
		}
	}
}

const char *LOG_SUBSYSTEM = NULL;

/*
 * Filters
 * By default DEBUG and INFO messages are disabled. If LOG_ENABLE_DEBUG is not
 * defined, then all log_debug() statements compile to zero-code and they cannot
 * be enabled on runtime.
 * To enable DEBUG or INFO messages at runtime, you can either specify that they
 * should be enabled globally, per file or specify a custom filter. Other
 * messages than DEBUG and INFO cannot be configured. However, additional
 * configuration options may be added later.
 *
 * Use log_set_config() to enable debug/info messages globally. If you
 * enable a global message type, then all other filters are skipped. If you
 * disable a global message type then fine-grained filters can take effect.
 *
 * To enable DEBUG/INFO messages for a specific source-file, you can add
 * this line to the top of the source file:
 *   #define LOG_CONFIG LOG_STATIC_CONFIG(true, true)
 * So info and debug messages are enabled for this file on compile-time. First
 * parameter of LOG_STATIC_CONFIG is for debug, second one for info.
 *
 * Or you can add new configurations on runtime. Runtime configurations take a
 * filter parameter and a config parameter. The filter specifies what messages
 * are affected and the config parameter specifies what action is performed.
 */

static struct log_config log__gconfig = {.sev = {
						 [LOG_DEBUG] = 0,
						 [LOG_INFO] = 0,
						 [LOG_NOTICE] = 1,
						 [LOG_WARNING] = 1,
						 [LOG_ERROR] = 1,
						 [LOG_CRITICAL] = 1,
						 [LOG_ALERT] = 1,
						 [LOG_FATAL] = 1,
					 }};

struct log_dynconf {
	struct log_dynconf *next;
	int handle;
	struct log_config config;
};

void log_set_config(const struct log_config *config)
{
	if (!config)
		return;

	log_lock();
	log__gconfig = *config;
	log_unlock();
}

static bool log__omit(const char *file, int line, const char *func, const char *subs,
		      enum log_severity sev)
{
	int val;

	if (sev >= LOG_SEV_NUM)
		return false;

	val = log__gconfig.sev[sev];
	if (val == 0)
		return true;
	if (val == 1)
		return false;

	return false;
}

/*
 * Forward declaration so we can use the locked-versions in other functions
 * here. Be careful to avoid deadlocks, though.
 * Also set default log-subsystem to "log" for all logging inside this API.
 */

static void log__submit(const char *file, int line, const char *func, const char *subs,
			unsigned int sev, const char *format, va_list args);

#define LOG_SUBSYSTEM "log"

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

static const char *log__sev2str[] = {
	[LOG_DEBUG] = "DEBUG",	   [LOG_INFO] = "INFO",	  [LOG_NOTICE] = "NOTICE",
	[LOG_WARNING] = "WARNING", [LOG_ERROR] = "ERROR", [LOG_CRITICAL] = "CRITICAL",
	[LOG_ALERT] = "ALERT",	   [LOG_FATAL] = "FATAL",
};

static void log__submit(const char *file, int line, const char *func, const char *subs,
			unsigned int sev, const char *format, va_list args)
{
	const char *prefix = NULL;
	FILE *out;
	long long sec, usec;
	bool nl;
	size_t len;

	if (log__omit(file, line, func, subs, sev))
		return;

	out = stderr;

	log__time(&sec, &usec);

	if (sev < LOG_SEV_NUM)
		prefix = log__sev2str[sev];

	if (prefix) {
		if (subs)
			fprintf(out, "[%.4lld.%.6lld] %s: %s: ", sec, usec, prefix, subs);
		else
			fprintf(out, "[%.4lld.%.6lld] %s: ", sec, usec, prefix);
	} else {
		if (subs)
			fprintf(out, "[%.4lld.%.6lld] %s: ", sec, usec, subs);
		else
			fprintf(out, "[%.4lld.%.6lld] ", sec, usec);
	}

	len = strlen(format);
	nl = format[len - 1] == '\n';

	if (!func)
		func = "<unknown>";
	if (!file)
		file = "<unknown>";
	if (line < 0)
		line = 0;

	vfprintf(out, format, args);

	if (!nl)
		fprintf(out, " (%s() in %s:%d)\n", func, file, line);
}

SHL_EXPORT
void log_submit(const char *file, int line, const char *func, const char *subs, unsigned int sev,
		const char *format, va_list args)
{
	int saved_errno = errno;

	log_lock();
	log__submit(file, line, func, subs, sev, format, args);
	log_unlock();

	errno = saved_errno;
}

SHL_EXPORT
void log_format(const char *file, int line, const char *func, const char *subs, unsigned int sev,
		const char *format, ...)
{
	va_list list;
	int saved_errno = errno;

	va_start(list, format);
	log_lock();
	log__submit(file, line, func, subs, sev, format, list);
	log_unlock();
	va_end(list);

	errno = saved_errno;
}

SHL_EXPORT
void log_llog(void *data, const char *file, int line, const char *func, const char *subs,
	      unsigned int sev, const char *format, va_list args)
{
	log_submit(file, line, func, subs, sev, format, args);
}

void log_print_init(const char *appname)
{
	if (!appname)
		appname = "<unknown>";
	log_format(LOG_DEFAULT_BASE, NULL, LOG_NOTICE, "%s Revision %s %s %s\n", appname,
		   shl_git_head, __DATE__, __TIME__);
}
