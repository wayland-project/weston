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

#include "zuc_junit_reporter.h"

#if ENABLE_JUNIT_XML

#include <fcntl.h>
#include <inttypes.h>
#include <libxml/parser.h>
#include <memory.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "zuc_event_listener.h"
#include "zuc_types.h"

#include <libweston/zalloc.h>

/**
 * Hardcoded output name.
 * @todo follow-up with refactoring to avoid filename hardcoding.
 * Will allow for better testing in parallel etc. in general.
 */
#define XML_FNAME "test_detail.xml"

#define ISO_8601_FORMAT "%Y-%m-%dT%H:%M:%SZ"

#if LIBXML_VERSION >= 20904
#define STRPRINTF_CAST
#else
#define STRPRINTF_CAST BAD_CAST
#endif

/**
 * Internal data.
 */
struct junit_data
{
	int fd;
	time_t begin;
};

#define MAX_64BIT_STRLEN 20

static void
set_attribute(xmlNodePtr node, const char *name, int value)
{
	xmlChar scratch[MAX_64BIT_STRLEN + 1] = {};
	xmlStrPrintf(scratch, sizeof(scratch), STRPRINTF_CAST "%d", value);
	xmlSetProp(node, BAD_CAST name, scratch);
}

/**
 * Output the given event.
 *
 * @param parent the parent node to add new content to.
 * @param event the event to write out.
 */
static void
emit_event(xmlNodePtr parent, struct zuc_event *event)
{
	char *msg = NULL;

	switch (event->op) {
	case ZUC_OP_TRUE:
		if (asprintf(&msg, "%s:%d: error: Value of: %s\n"
			     "  Actual: false\n"
			     "Expected: true\n", event->file, event->line,
			     event->expr1) < 0) {
			msg = NULL;
		}
		break;
	case ZUC_OP_FALSE:
		if (asprintf(&msg, "%s:%d: error: Value of: %s\n"
			     "  Actual: true\n"
			     "Expected: false\n", event->file, event->line,
			     event->expr1) < 0) {
			msg = NULL;
		}
		break;
	case ZUC_OP_NULL:
		if (asprintf(&msg, "%s:%d: error: Value of: %s\n"
			     "  Actual: %p\n"
			     "Expected: %p\n", event->file, event->line,
			     event->expr1, (void *)event->val1, NULL) < 0) {
			msg = NULL;
		}
		break;
	case ZUC_OP_NOT_NULL:
		if (asprintf(&msg, "%s:%d: error: Value of: %s\n"
			     "  Actual: %p\n"
			     "Expected: not %p\n", event->file, event->line,
			     event->expr1, (void *)event->val1, NULL) < 0) {
			msg = NULL;
		}
		break;
	case ZUC_OP_EQ:
		if (event->valtype == ZUC_VAL_CSTR) {
			if (asprintf(&msg, "%s:%d: error: Value of: %s\n"
				     "  Actual: %s\n"
				     "Expected: %s\n"
				     "Which is: %s\n",
				     event->file, event->line, event->expr2,
				     (char *)event->val2, event->expr1,
				     (char *)event->val1) < 0) {
				msg = NULL;
			}
		} else {
			if (asprintf(&msg, "%s:%d: error: Value of: %s\n"
			             "  Actual: %"PRIdPTR"\n"
			             "Expected: %s\n"
			             "Which is: %"PRIdPTR"\n",
			             event->file, event->line, event->expr2,
			             event->val2, event->expr1,
			             event->val1) < 0) {
				msg = NULL;
			}
		}
		break;
	case ZUC_OP_NE:
		if (event->valtype == ZUC_VAL_CSTR) {
			if (asprintf(&msg, "%s:%d: error: "
				     "Expected: (%s) %s (%s),"
				     " actual: %s == %s\n",
				     event->file, event->line,
				     event->expr1, zuc_get_opstr(event->op),
				     event->expr2, (char *)event->val1,
				     (char *)event->val2) < 0) {
				msg = NULL;
			}
		} else {
			if (asprintf(&msg, "%s:%d: error: "
			             "Expected: (%s) %s (%s),"
			             " actual: %"PRIdPTR" vs %"PRIdPTR"\n",
			             event->file, event->line,
			             event->expr1, zuc_get_opstr(event->op),
			             event->expr2, event->val1,
			             event->val2) < 0) {
				msg = NULL;
			}
		}
		break;
	case ZUC_OP_TERMINATE:
	{
		char const *level = (event->val1 == 0) ? "error"
			: (event->val1 == 1) ? "warning"
			: "note";
		if (asprintf(&msg, "%s:%d: %s: %s\n",
			     event->file, event->line, level,
			     event->expr1) < 0) {
			msg = NULL;
		}
		break;
	}
	case ZUC_OP_TRACEPOINT:
		if (asprintf(&msg, "%s:%d: note: %s\n",
			     event->file, event->line, event->expr1) < 0) {
			msg = NULL;
		}
		break;
	default:
		if (asprintf(&msg, "%s:%d: error: "
		             "Expected: (%s) %s (%s), actual: %"PRIdPTR" vs "
		             "%"PRIdPTR"\n",
		             event->file, event->line,
		             event->expr1, zuc_get_opstr(event->op),
		             event->expr2, event->val1, event->val2) < 0) {
			msg = NULL;
		}
	}

	if ((event->op == ZUC_OP_TERMINATE) && (event->val1 > 1)) {
		xmlNewChild(parent, NULL, BAD_CAST "skipped", NULL);
	} else {
		xmlNodePtr node = xmlNewChild(parent, NULL,
					      BAD_CAST "failure", NULL);

		if (msg) {
			xmlSetProp(node, BAD_CAST "message", BAD_CAST msg);
		}
		xmlSetProp(node, BAD_CAST "type", BAD_CAST "");
		if (msg) {
			xmlNodePtr cdata = xmlNewCDataBlock(node->doc,
							    BAD_CAST msg,
							    strlen(msg));
			xmlAddChild(node, cdata);
		}
	}

	free(msg);
}

/**
 * Formats a time in milliseconds to the normal JUnit elapsed form, or
 * NULL if there is a problem.
 * The caller should release this with free()
 *
 * @return the formatted time string upon success, NULL otherwise.
 */
static char *
as_duration(long ms)
{
	char *str = NULL;

	if (asprintf(&str, "%1.3f", ms / 1000.0) < 0) {
		str = NULL;
	} else {
		/*
		 * Special case to match behavior of standard JUnit output
		 * writers. Assumption is certain readers might have
		 * limitations, etc. so it is best to keep 100% identical
		 * output.
		 */
		if (!strcmp("0.000", str)) {
			free(str);
			str = strdup("0");
		}
	}
	return str;
}

/**
 * Returns the status string for the tests (run/notrun).
 *
 * @param test the test to check status of.
 * @return the status string.
 */
static char const *
get_test_status(struct zuc_test *test)
{
	if (test->disabled || test->skipped)
		return "notrun";
	else
		return "run";
}

/**
 * Output the given test.
 *
 * @param parent the parent node to add new content to.
 * @param test the test to write out.
 */
static void
emit_test(xmlNodePtr parent, struct zuc_test *test)
{
	char *time_str = as_duration(test->elapsed);
	xmlNodePtr node = xmlNewChild(parent, NULL, BAD_CAST "testcase", NULL);

	xmlSetProp(node, BAD_CAST "name", BAD_CAST test->name);
	xmlSetProp(node, BAD_CAST "status", BAD_CAST get_test_status(test));

	if (time_str) {
		xmlSetProp(node, BAD_CAST "time", BAD_CAST time_str);

		free(time_str);
		time_str = NULL;
	}

	xmlSetProp(node, BAD_CAST "classname", BAD_CAST test->test_case->name);

	if ((test->failed || test->fatal || test->skipped) && test->events) {
		struct zuc_event *evt;
		for (evt = test->events; evt; evt = evt->next)
			emit_event(node, evt);
	}
}

/**
 * Output the given test case.
 *
 * @param parent the parent node to add new content to.
 * @param test_case the test case to write out.
 */
static void
emit_case(xmlNodePtr parent, struct zuc_case *test_case)
{
	int i;
	int skipped = 0;
	int disabled = 0;
	int failures = 0;
	xmlNodePtr node = NULL;
	char *time_str = as_duration(test_case->elapsed);

	for (i = 0; i < test_case->test_count; ++i) {
		if (test_case->tests[i]->disabled )
			disabled++;
		if (test_case->tests[i]->skipped )
			skipped++;
		if (test_case->tests[i]->failed
		    || test_case->tests[i]->fatal )
			failures++;
	}

	node = xmlNewChild(parent, NULL, BAD_CAST "testsuite", NULL);
	xmlSetProp(node, BAD_CAST "name", BAD_CAST test_case->name);

	set_attribute(node, "tests", test_case->test_count);
	set_attribute(node, "failures", failures);
	set_attribute(node, "disabled", disabled);
	set_attribute(node, "skipped", skipped);

	if (time_str) {
		xmlSetProp(node, BAD_CAST "time", BAD_CAST time_str);
		free(time_str);
		time_str = NULL;
	}

	for (i = 0; i < test_case->test_count; ++i)
		emit_test(node, test_case->tests[i]);
}

/**
 * Formats a time in milliseconds to the full ISO-8601 date/time string
 * format, or NULL if there is a problem.
 * The caller should release this with free()
 *
 * @return the formatted time string upon success, NULL otherwise.
 */
static char *
as_iso_8601(time_t const *t)
{
	char *result = NULL;
	char buf[32] = {};
	struct tm when;

	if (gmtime_r(t, &when) != NULL)
		if (strftime(buf, sizeof(buf), ISO_8601_FORMAT, &when))
			result = strdup(buf);

	return result;
}


static void
run_started(void *data, int live_case_count, int live_test_count,
	    int disabled_count)
{
	struct junit_data *jdata = data;

	jdata->begin = time(NULL);
	jdata->fd = open(XML_FNAME, O_WRONLY | O_CLOEXEC | O_CREAT | O_TRUNC,
			 S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
}

static void
run_ended(void *data, int case_count, struct zuc_case **cases,
	  int live_case_count, int live_test_count, int total_passed,
	  int total_failed, int total_disabled, long total_elapsed)
{
	int i;
	long time = 0;
	char *time_str = NULL;
	char *timestamp = NULL;
	xmlNodePtr root = NULL;
	xmlDocPtr doc = NULL;
	xmlChar *xmlchars = NULL;
	int xmlsize = 0;
	struct junit_data *jdata = data;

	for (i = 0; i < case_count; ++i)
		time += cases[i]->elapsed;

	time_str = as_duration(time);
	timestamp = as_iso_8601(&jdata->begin);

	/* here would be where to add errors? */

	doc = xmlNewDoc(BAD_CAST "1.0");
	root = xmlNewNode(NULL, BAD_CAST "testsuites");
	xmlDocSetRootElement(doc, root);

	set_attribute(root, "tests", live_test_count);
	set_attribute(root, "failures", total_failed);
	set_attribute(root, "disabled", total_disabled);

	if (timestamp) {
		xmlSetProp(root, BAD_CAST "timestamp", BAD_CAST timestamp);
		free(timestamp);
		timestamp = NULL;
	}

	if (time_str) {
		xmlSetProp(root, BAD_CAST "time", BAD_CAST time_str);
		free(time_str);
		time_str = NULL;
	}

	xmlSetProp(root, BAD_CAST "name", BAD_CAST "AllTests");

	for (i = 0; i < case_count; ++i) {
		emit_case(root, cases[i]);
	}

	xmlDocDumpFormatMemoryEnc(doc, &xmlchars, &xmlsize, "UTF-8", 1);
	dprintf(jdata->fd, "%s", (char *) xmlchars);
	xmlFree(xmlchars);
	xmlchars = NULL;
	xmlFreeDoc(doc);

	if ((jdata->fd != fileno(stdout))
	    && (jdata->fd != fileno(stderr))
	    && (jdata->fd != -1)) {
		close(jdata->fd);
		jdata->fd = -1;
	}
}

static void
destroy(void *data)
{
	xmlCleanupParser();

	free(data);
}

struct zuc_event_listener *
zuc_junit_reporter_create(void)
{
	struct zuc_event_listener *listener =
		zalloc(sizeof(struct zuc_event_listener));
	if (!listener)
		return NULL;

	struct junit_data *data = zalloc(sizeof(struct junit_data));
	if (data) {
		data->fd = -1;
	}

	listener->data = data;
	listener->destroy = destroy;
	listener->run_started = run_started;
	listener->run_ended = run_ended;

	return listener;
}

#else /* ENABLE_JUNIT_XML */

#include <stddef.h>
#include "zuc_event_listener.h"

/*
 * Simple stub version if junit output (including libxml2 support) has
 * been disabled.
 * Will return NULL to cause failures as calling this when the #define
 * has not been enabled is an invalid scenario.
 */

struct zuc_event_listener *
zuc_junit_reporter_create(void)
{
	return NULL;
}

#endif /* ENABLE_JUNIT_XML */
