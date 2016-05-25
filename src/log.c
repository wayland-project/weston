/*
 * Copyright © 2012 Martin Minarik
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <wayland-util.h>

#include "compositor.h"

#include "os-compatibility.h"

static FILE *weston_logfile = NULL;

static int cached_tm_mday = -1;

static int
weston_log_timestamp(void)
{
	struct timeval tv;
	struct tm *brokendown_time;
	char string[128];

	gettimeofday(&tv, NULL);

	brokendown_time = localtime(&tv.tv_sec);
	if (brokendown_time == NULL)
		return fprintf(weston_logfile, "[(NULL)localtime] ");

	if (brokendown_time->tm_mday != cached_tm_mday) {
		strftime(string, sizeof string, "%Y-%m-%d %Z", brokendown_time);
		fprintf(weston_logfile, "Date: %s\n", string);

		cached_tm_mday = brokendown_time->tm_mday;
	}

	strftime(string, sizeof string, "%H:%M:%S", brokendown_time);

	return fprintf(weston_logfile, "[%s.%03li] ", string, tv.tv_usec/1000);
}

static void
custom_handler(const char *fmt, va_list arg)
{
	weston_log_timestamp();
	fprintf(weston_logfile, "libwayland: ");
	vfprintf(weston_logfile, fmt, arg);
}

void
weston_log_file_open(const char *filename)
{
	wl_log_set_handler_server(custom_handler);

	if (filename != NULL) {
		weston_logfile = fopen(filename, "a");
		if (weston_logfile)
			os_fd_set_cloexec(fileno(weston_logfile));
	}

	if (weston_logfile == NULL)
		weston_logfile = stderr;
	else
		setvbuf(weston_logfile, NULL, _IOLBF, 256);
}

void
weston_log_file_close()
{
	if ((weston_logfile != stderr) && (weston_logfile != NULL))
		fclose(weston_logfile);
	weston_logfile = stderr;
}

WL_EXPORT int
weston_vlog(const char *fmt, va_list ap)
{
	int l;

	l = weston_log_timestamp();
	l += vfprintf(weston_logfile, fmt, ap);

	return l;
}

WL_EXPORT int
weston_log(const char *fmt, ...)
{
	int l;
	va_list argp;

	va_start(argp, fmt);
	l = weston_vlog(fmt, argp);
	va_end(argp);

	return l;
}

WL_EXPORT int
weston_vlog_continue(const char *fmt, va_list argp)
{
	return vfprintf(weston_logfile, fmt, argp);
}

WL_EXPORT int
weston_log_continue(const char *fmt, ...)
{
	int l;
	va_list argp;

	va_start(argp, fmt);
	l = weston_vlog_continue(fmt, argp);
	va_end(argp);

	return l;
}
