/*
 * Copyright © 2015 Samsung Electronics Co., Ltd
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

#include "zuc_collector.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libweston/zalloc.h>
#include "zuc_event_listener.h"
#include "zunitc/zunitc_impl.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/**
 * @file
 * General handling of collecting events during testing to pass back to
 * main tracking of fork()'d tests.
 *
 * @note implementation of zuc_process_message() is included here so that
 * all child->parent IPC is in a single source file for easier maintenance
 * and updating.
 */

/**
 * Internal data struct for processing.
 */
struct collector_data
{
	int *fd;		/**< file descriptor to output to. */
	struct zuc_test *test;	/**< current test, or NULL. */
};

/**
 * Stores an int32_t into the given buffer.
 *
 * @param ptr the buffer to store to.
 * @param val the value to store.
 * @return a pointer to the position in the buffer after the stored value.
 */
static char *
pack_int32(char *ptr, int32_t val);

/**
 * Stores an intptr_t into the given buffer.
 *
 * @param ptr the buffer to store to.
 * @param val the value to store.
 * @return a pointer to the position in the buffer after the stored value.
 */
static char *
pack_intptr_t(char *ptr, intptr_t val);

/**
 * Extracts a int32_t from the given buffer.
 *
 * @param ptr the buffer to extract from.
 * @param val the value to set.
 * @return a pointer to the position in the buffer after the extracted
 * value.
 */
static char const *
unpack_int32(char const *ptr, int32_t *val);

/**
 * Extracts a intptr_t from the given buffer.
 *
 * @param ptr the buffer to extract from.
 * @param val the value to set.
 * @return a pointer to the position in the buffer after the extracted
 * value.
 */
static char const *
unpack_intptr_t(char const *ptr, intptr_t *val);

/**
 * Extracts a length-prefixed string from the given buffer.
 *
 * @param ptr the buffer to extract from.
 * @param str the value to set.
 * @return a pointer to the position in the buffer after the extracted
 * value.
 */
static char const *
unpack_string(char const *ptr, char **str);

/**
 * Extracts an event from the given buffer.
 *
 * @param ptr the buffer to extract from.
 * @param len the length of the given buffer.
 * @return an event that was packed in the buffer
 */
static struct zuc_event *
unpack_event(char const *ptr, int32_t len);

/**
 * Handles an event by either attaching it directly or sending it over IPC
 * as needed.
 */
static void
store_event(struct collector_data *cdata,
	    enum zuc_event_type event_type, char const *file, int line,
	    enum zuc_fail_state state, enum zuc_check_op op,
	    enum zuc_check_valtype valtype,
	    intptr_t val1, intptr_t val2, const char *expr1, const char *expr2);

static void
destroy(void *data);

static void
test_started(void *data, struct zuc_test *test);

static void
test_ended(void *data, struct zuc_test *test);

static void
check_triggered(void *data, char const *file, int line,
		enum zuc_fail_state state, enum zuc_check_op op,
		enum zuc_check_valtype valtype,
		intptr_t val1, intptr_t val2,
		const char *expr1, const char *expr2);

static void
collect_event(void *data, char const *file, int line, const char *expr1);

struct zuc_event_listener *
zuc_collector_create(int *pipe_fd)
{
	struct zuc_event_listener *listener =
		zalloc(sizeof(struct zuc_event_listener));
	if (!listener)
		return NULL;

	listener->data = zalloc(sizeof(struct collector_data));
	((struct collector_data *)listener->data)->fd = pipe_fd;
	listener->destroy = destroy;
	listener->test_started = test_started;
	listener->test_ended = test_ended;
	listener->check_triggered = check_triggered;
	listener->collect_event = collect_event;

	return listener;
}

char *
pack_int32(char *ptr, int32_t val)
{
	memcpy(ptr, &val, sizeof(val));
	return ptr + sizeof(val);
}

char *
pack_intptr_t(char *ptr, intptr_t val)
{
	memcpy(ptr, &val, sizeof(val));
	return ptr + sizeof(val);
}

static char *
pack_cstr(char *ptr, intptr_t val, int len)
{
	if (val == 0) { /* a null pointer */
		ptr = pack_int32(ptr, -1);
	} else {
		ptr = pack_int32(ptr, len);
		memcpy(ptr, (const char *)val, len);
		ptr += len;
	}
	return ptr;
}

void
destroy(void *data)
{
	free(data);
}

void
test_started(void *data, struct zuc_test *test)
{
	struct collector_data *cdata = data;
	cdata->test = test;
}

void
test_ended(void *data, struct zuc_test *test)
{
	struct collector_data *cdata = data;
	cdata->test = NULL;
}

void
check_triggered(void *data, char const *file, int line,
		enum zuc_fail_state state, enum zuc_check_op op,
		enum zuc_check_valtype valtype,
		intptr_t val1, intptr_t val2,
		const char *expr1, const char *expr2)
{
	struct collector_data *cdata = data;
	if (op != ZUC_OP_TRACEPOINT)
		store_event(cdata, ZUC_EVENT_IMMEDIATE, file, line, state, op,
			    valtype,
			    val1, val2, expr1, expr2);
}

void
collect_event(void *data, char const *file, int line, const char *expr1)
{
	struct collector_data *cdata = data;
	store_event(cdata, ZUC_EVENT_DEFERRED, file, line, ZUC_CHECK_OK,
		    ZUC_OP_TRACEPOINT, ZUC_VAL_INT,
		    0, 0, expr1, "");
}

void
store_event(struct collector_data *cdata,
	    enum zuc_event_type event_type, char const *file, int line,
	    enum zuc_fail_state state, enum zuc_check_op op,
	    enum zuc_check_valtype valtype,
	    intptr_t val1, intptr_t val2, const char *expr1, const char *expr2)
{
	struct zuc_event *event = zalloc(sizeof(*event));
	if (!event)
		return;

	event->file = strdup(file);
	event->line = line;
	event->state = state;
	event->op = op;
	event->valtype = valtype;
	event->val1 = val1;
	event->val2 = val2;
	if (valtype == ZUC_VAL_CSTR) {
		if (val1)
			event->val1 = (intptr_t)strdup((const char *)val1);
		if (val2)
			event->val2 = (intptr_t)strdup((const char *)val2);
	}
	event->expr1 = strdup(expr1);
	event->expr2 = strdup(expr2);

	zuc_attach_event(cdata->test, event, event_type, false);

	if (*cdata->fd == -1) {
	} else {
		/* Need to pass it back */
		int sent;
		int count;
		int expr1_len = strlen(expr1);
		int expr2_len = strlen(expr2);
		int val1_len =
			((valtype == ZUC_VAL_CSTR) && val1) ?
			strlen((char *)val1) : 0;
		int val2_len =
			((valtype == ZUC_VAL_CSTR) && val2) ?
			strlen((char *)val2) : 0;
		int file_len = strlen(file);
		int len = (sizeof(int32_t) * 9) + file_len
			+ (sizeof(intptr_t) * 2)
			+ ((valtype == ZUC_VAL_CSTR) ?
			   (sizeof(int32_t) * 2) + val1_len + val2_len : 0)
			+ expr1_len + expr2_len;
		char *buf = zalloc(len);

		char *ptr = pack_int32(buf, len - 4);
		ptr = pack_int32(ptr, event_type);
		ptr = pack_int32(ptr, file_len);
		memcpy(ptr, file, file_len);
		ptr += file_len;
		ptr = pack_int32(ptr, line);
		ptr = pack_int32(ptr, state);
		ptr = pack_int32(ptr, op);
		ptr = pack_int32(ptr, valtype);
		if (valtype == ZUC_VAL_CSTR) {
			ptr = pack_cstr(ptr, val1, val1_len);
			ptr = pack_cstr(ptr, val2, val2_len);
		} else {
			ptr = pack_intptr_t(ptr, val1);
			ptr = pack_intptr_t(ptr, val2);
		}

		ptr = pack_int32(ptr, expr1_len);
		if (expr1_len) {
			memcpy(ptr, expr1, expr1_len);
			ptr += expr1_len;
		}
		ptr = pack_int32(ptr, expr2_len);
		if (expr2_len) {
			memcpy(ptr, expr2, expr2_len);
			ptr += expr2_len;
		}


		sent = 0;
		while (sent < len) {
			count = write(*cdata->fd, buf, len);
			if (count == -1)
				break;
			sent += count;
		}

		free(buf);
	}
}

char const *
unpack_int32(char const *ptr, int32_t *val)
{
	memcpy(val, ptr, sizeof(*val));
	return ptr + sizeof(*val);
}

char const *
unpack_intptr_t(char const *ptr, intptr_t *val)
{
	memcpy(val, ptr, sizeof(*val));
	return ptr + sizeof(*val);
}

char const *
unpack_string(char const *ptr, char **str)
{
	int32_t len = 0;
	ptr = unpack_int32(ptr, &len);
	*str = zalloc(len + 1);
	if (len)
		memcpy(*str, ptr, len);
	ptr += len;
	return ptr;
}

static char const *
unpack_cstr(char const *ptr, char **str)
{
	int32_t len = 0;
	ptr = unpack_int32(ptr, &len);
	if (len < 0) {
		*str = NULL;
	} else {
		*str = zalloc(len + 1);
		if (len)
			memcpy(*str, ptr, len);
		ptr += len;
	}
	return ptr;
}

struct zuc_event *
unpack_event(char const *ptr, int32_t len)
{
	int32_t val = 0;
	struct zuc_event *evt = zalloc(sizeof(*evt));
	if (!evt)
		return NULL;

	char const *tmp = unpack_string(ptr, &evt->file);
	tmp = unpack_int32(tmp, &evt->line);

	tmp = unpack_int32(tmp, &val);
	evt->state = val;
	tmp = unpack_int32(tmp, &val);
	evt->op = val;
	tmp = unpack_int32(tmp, &val);
	evt->valtype = val;

	if (evt->valtype == ZUC_VAL_CSTR) {
		char *ptr = NULL;
		tmp = unpack_cstr(tmp, &ptr);
		evt->val1 = (intptr_t)ptr;
		tmp = unpack_cstr(tmp, &ptr);
		evt->val2 = (intptr_t)ptr;
	} else {
		tmp = unpack_intptr_t(tmp, &evt->val1);
		tmp = unpack_intptr_t(tmp, &evt->val2);
	}

	tmp = unpack_string(tmp, &evt->expr1);
	tmp = unpack_string(tmp, &evt->expr2);

	return evt;
}

int
zuc_process_message(struct zuc_test *test, int fd)
{
	char buf[4] = {};
	int got = read(fd, buf, 4);
	if (got == 4) {
		enum zuc_event_type event_type = ZUC_EVENT_IMMEDIATE;
		int32_t val = 0;
		int32_t len = 0;
		const char *tmp = NULL;
		char *raw = NULL;
		unpack_int32(buf, &len);
		raw = zalloc(len);
		got = read(fd, raw, len);

		tmp = unpack_int32(raw, &val);
		event_type = val;

		struct zuc_event *evt = unpack_event(tmp, len - (tmp - raw));
		zuc_attach_event(test, evt, event_type, true);
		free(raw);
	}
	return got;
}
