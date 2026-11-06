/*
 * Log/Debug Interface
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Dedicated to the Public Domain
 */

/*
 * Log/Debug Interface
 * This interface provides basic logging to stderr. By default,
 * all log-messages are forwarded to stderr but you can change this to an
 * arbitrary file. However, no complex file-rotation/backup functions are
 * supported so you should use the default (stderr) and use a proper init-system
 * like systemd to do log-rotations. This can also forward stderr messages into
 * log-files.
 *
 * Define BUILD_ENABLE_DEBUG before including this header to enable
 * debug-messages for this file.
 */

#ifndef SHL_LOG_H_INCLUDED
#define SHL_LOG_H_INCLUDED

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>

/*
 * Log Messages and Filters
 * A log message consists of:
 *  - level: the log level
 *  - subs: the subsystem
 *  - format: format string
 *  - args: arguments depending on format string
 *
 * Use log_set_level() to set the log level, messages with a high severity
 * than the current level are discarded.
 * The log level is the same as the Linux kernel and systemd.
 */
enum log_severity {
	LOG_FATAL,
	LOG_ALERT,
	LOG_CRITICAL,
	LOG_ERROR,
	LOG_WARNING,
	LOG_NOTICE,
	LOG_INFO,
	LOG_DEBUG,
	LOG_SEV_NUM,
};

void log_set_level(enum log_severity level);

/*
 * Log-Functions
 * These functions pass a log-message to the log-subsystem. Handy helpers are
 * provided below. You almost never use these directly.
 *
 * log_submit:
 * Submit the message to the log-subsystem. This is the backend of all other
 * loggers.
 *
 * log_format:
 * Same as log_submit but first converts the arguments into a va_list object.
 *
 * log_print_init(appname):
 * This prints a message with build-time/date and appname to the log. You should
 * invoke this very early in your program. It is not required, though.
 */

__attribute__((format(printf, 3, 0))) void log_submit(enum log_severity level, const char *subs,
						      const char *format, va_list args);

__attribute__((format(printf, 3, 4))) void log_format(enum log_severity level, const char *subs,
						      const char *format, ...);

void log_print_init(const char *appname);

static inline __attribute__((format(printf, 2, 3))) void log_dummyf(enum log_severity level,
								    const char *format, ...)
{
}

/*
 * Default values
 * The subsystem is by default an empty string. To overwrite this, add this
 * line to the top of your source file:
 *   #define LOG_SUBSYSTEM "mysubsystem"
 * Then all following log-messages will use this string as subsystem.
 */

#ifndef LOG_CONFIG
#define LOG_CONFIG NULL
#endif

#ifndef LOG_SUBSYSTEM
extern const char *LOG_SUBSYSTEM;
#endif

#define log_printf(level, format, ...) log_format((level), LOG_SUBSYSTEM, (format), ##__VA_ARGS__)

/*
 * Helpers
 * The pick-up all the default values and submit the message to the
 * log-subsystem. The log_debug() function produces zero-code if
 * BUILD_ENABLE_DEBUG is not defined. Therefore, it can be heavily used for
 * debugging and will not have any side-effects.
 */

#ifdef BUILD_ENABLE_DEBUG
#define log_debug(format, ...) log_printf(LOG_DEBUG, (format), ##__VA_ARGS__)
#else
#define log_debug(format, ...) log_dummyf(LOG_DEBUG, (format), ##__VA_ARGS__)
#endif

#define log_info(format, ...) log_printf(LOG_INFO, (format), ##__VA_ARGS__)
#define log_notice(format, ...) log_printf(LOG_NOTICE, (format), ##__VA_ARGS__)
#define log_warning(format, ...) log_printf(LOG_WARNING, (format), ##__VA_ARGS__)
#define log_error(format, ...) log_printf(LOG_ERROR, (format), ##__VA_ARGS__)
#define log_critical(format, ...) log_printf(LOG_CRITICAL, (format), ##__VA_ARGS__)
#define log_alert(format, ...) log_printf(LOG_ALERT, (format), ##__VA_ARGS__)
#define log_fatal(format, ...) log_printf(LOG_FATAL, (format), ##__VA_ARGS__)

#define log_dbg log_debug
#define log_warn log_warning
#define log_err log_error
#define log_crit log_critical

#endif /* SHL_LOG_H_INCLUDED */
