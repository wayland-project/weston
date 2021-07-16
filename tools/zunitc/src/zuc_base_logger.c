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

#include "zuc_base_logger.h"

#include <inttypes.h>
#include <memory.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "zuc_event_listener.h"
#include "zuc_types.h"

#include <libweston/zalloc.h>

/* a few sequences for rudimentary ANSI graphics. */
#define CSI_GRN "\x1b[0;32m"
#define CSI_RED "\x1b[0;31m"
#define CSI_YLW "\x1b[0;33m"
#define CSI_RST "\x1b[m"

/**
 * Logical mappings of style levels.
 */
enum style_level {
	STYLE_GOOD,
	STYLE_WARN,
	STYLE_BAD
};

/**
 * Structure for internal context.
 */
struct base_data {
	bool use_color;
};

/**
 * Prints a formatted string with optional ANSI coloring.
 *
 * @param use_color true to colorize the output, false to output normally.
 * @param slevel the logical type to color for.
 * @param fmt the format string to print with.
 */
static void
styled_printf(bool use_color, enum style_level slevel, const char *fmt, ...);

static void
destroy(void *data);

static void
pre_run(void *data, int pass_count, int pass_num, int seed, const char *filter);

static void
run_started(void *data, int live_case_count,
	    int live_test_count, int disabled_count);

static void
run_ended(void *data, int case_count, struct zuc_case **cases,
	  int live_case_count, int live_test_count, int total_passed,
	  int total_failed, int total_disabled, long total_elapsed);

static void
case_started(void *data, struct zuc_case *test_case, int live_test_count,
	     int disabled_count);

static void
case_ended(void *data, struct zuc_case *test_case);

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

struct zuc_event_listener *
zuc_base_logger_create(void)
{
	struct zuc_event_listener *listener =
		zalloc(sizeof(struct zuc_event_listener));
	if (!listener)
		return NULL;

	listener->data = zalloc(sizeof(struct base_data));
	listener->destroy = destroy;
	listener->pre_run = pre_run;
	listener->run_started = run_started;
	listener->run_ended = run_ended;
	listener->case_started = case_started;
	listener->case_ended = case_ended;
	listener->test_started = test_started;
	listener->test_ended = test_ended;
	listener->check_triggered = check_triggered;

	return listener;
}

void
styled_printf(bool use_color, enum style_level slevel, const char *fmt, ...)
{
	va_list argp;

	if (use_color)
		switch (slevel) {
		case STYLE_GOOD:
			printf(CSI_GRN);
			break;
		case STYLE_WARN:
			printf(CSI_YLW);
			break;
		case STYLE_BAD:
			printf(CSI_RED);
			break;
		default:
			break;
		}

	va_start(argp, fmt);
	vprintf(fmt, argp);
	va_end(argp);

	if (use_color)
		printf(CSI_RST);
}

void
destroy(void *data)
{
	free(data);
}

void
pre_run(void *data, int pass_count, int pass_num, int seed, const char *filter)
{
	struct base_data *bdata = data;

	bdata->use_color = isatty(fileno(stdout))
		&& getenv("TERM") && strcmp(getenv("TERM"), "dumb");

	if (pass_count > 1)
		printf("\nRepeating all tests (iteration %d) . . .\n\n",
		       pass_num);

	if (filter && filter[0])
		styled_printf(bdata->use_color, STYLE_WARN,
			      "Note: test filter = %s\n",
			      filter);

	if (seed > 0)
		styled_printf(bdata->use_color, STYLE_WARN,
			      "Note: Randomizing tests' orders"
			      " with a seed of %u .\n",
			      seed);
}

void
run_started(void *data, int live_case_count,
	    int live_test_count, int disabled_count)
{
	struct base_data *bdata = data;

	styled_printf(bdata->use_color, STYLE_GOOD, "[==========]");
	printf(" Running %d %s from %d test %s.\n",
	       live_test_count,
	       (live_test_count == 1) ? "test" : "tests",
	       live_case_count,
	       (live_case_count == 1) ? "case" : "cases");
}

void
run_ended(void *data, int case_count, struct zuc_case **cases,
	  int live_case_count, int live_test_count, int total_passed,
	  int total_failed, int total_disabled, long total_elapsed)
{
	struct base_data *bdata = data;
	styled_printf(bdata->use_color, STYLE_GOOD, "[==========]");
	printf(" %d %s from %d test %s ran. (%ld ms)\n",
	       live_test_count,
	       (live_test_count == 1) ? "test" : "tests",
	       live_case_count,
	       (live_case_count == 1) ? "case" : "cases",
	       total_elapsed);

	if (total_passed) {
		styled_printf(bdata->use_color, STYLE_GOOD, "[  PASSED  ]");
		printf(" %d %s.\n", total_passed,
		       (total_passed == 1) ? "test" : "tests");
	}

	if (total_failed) {
		int case_num;
		styled_printf(bdata->use_color, STYLE_BAD, "[  FAILED  ]");
		printf(" %d %s, listed below:\n",
		       total_failed, (total_failed == 1) ? "test" : "tests");

		for (case_num = 0; case_num < case_count; ++case_num) {
			int i;
			for (i = 0; i < cases[case_num]->test_count; ++i) {
				struct zuc_test *curr =
					cases[case_num]->tests[i];
				if (curr->failed || curr->fatal) {
					styled_printf(bdata->use_color,
						      STYLE_BAD,
						      "[  FAILED  ]");
					printf(" %s.%s\n",
					       cases[case_num]->name,
					       curr->name);
				}
			}
		}
	}

	if (total_failed || total_disabled)
		printf("\n");

	if (total_failed)
		printf(" %d FAILED %s\n",
		       total_failed,
		       (total_failed == 1) ? "TEST" : "TESTS");

	if (total_disabled)
		styled_printf(bdata->use_color, STYLE_WARN,
			      "  YOU HAVE %d DISABLED %s\n",
			      total_disabled,
			      (total_disabled == 1) ? "TEST" : "TESTS");
}

void
case_started(void *data, struct zuc_case *test_case, int live_test_count,
	     int disabled_count)
{
	struct base_data *bdata = data;
	styled_printf(bdata->use_color, STYLE_GOOD, "[----------]");
	printf(" %d %s from %s.\n",
	       live_test_count,
	       (live_test_count == 1) ? "test" : "tests",
	       test_case->name);

}

void
case_ended(void *data, struct zuc_case *test_case)
{
	struct base_data *bdata = data;
	styled_printf(bdata->use_color, STYLE_GOOD, "[----------]");
	printf(" %d %s from %s (%ld ms)\n",
	       test_case->test_count,
	       (test_case->test_count == 1) ? "test" : "tests",
	       test_case->name,
	       test_case->elapsed);
	printf("\n");
}

void
test_started(void *data, struct zuc_test *test)
{
	struct base_data *bdata = data;
	styled_printf(bdata->use_color, STYLE_GOOD, "[ RUN      ]");
	printf(" %s.%s\n", test->test_case->name, test->name);
}

void
test_ended(void *data, struct zuc_test *test)
{
	struct base_data *bdata = data;
	if (test->failed || test->fatal) {
		styled_printf(bdata->use_color, STYLE_BAD, "[  FAILED  ]");
		printf(" %s.%s (%ld ms)\n",
		       test->test_case->name, test->name, test->elapsed);
	} else {
		styled_printf(bdata->use_color, STYLE_GOOD, "[       OK ]");
		printf(" %s.%s (%ld ms)\n",
		       test->test_case->name, test->name, test->elapsed);
	}
}

const char *
zuc_get_opstr(enum zuc_check_op op)
{
	switch (op) {
	case ZUC_OP_EQ:
		return "=";
	case ZUC_OP_NE:
		return "!=";
	case ZUC_OP_GE:
		return ">=";
	case ZUC_OP_GT:
		return ">";
	case ZUC_OP_LE:
		return "<=";
	case ZUC_OP_LT:
		return "<";
	default:
		return "???";
	}
}

void
check_triggered(void *data, char const *file, int line,
		enum zuc_fail_state state, enum zuc_check_op op,
		enum zuc_check_valtype valtype,
		intptr_t val1, intptr_t val2,
		const char *expr1, const char *expr2)
{
	switch (op) {
	case ZUC_OP_TRUE:
		printf("%s:%d: error: Value of: %s\n", file, line, expr1);
		printf("  Actual: false\n");
		printf("Expected: true\n");
		break;
	case ZUC_OP_FALSE:
		printf("%s:%d: error: Value of: %s\n", file, line, expr1);
		printf("  Actual: true\n");
		printf("Expected: false\n");
		break;
	case ZUC_OP_NULL:
		printf("%s:%d: error: Value of: %s\n", file, line, expr1);
		printf("  Actual: %p\n", (void *)val1);
		printf("Expected: %p\n", NULL);
		break;
	case ZUC_OP_NOT_NULL:
		printf("%s:%d: error: Value of: %s\n", file, line, expr1);
		printf("  Actual: %p\n", (void *)val1);
		printf("Expected: not %p\n", NULL);
		break;
	case ZUC_OP_EQ:
		if (valtype == ZUC_VAL_CSTR) {
			printf("%s:%d: error: Value of: %s\n", file, line,
			       expr2);
			printf("  Actual: %s\n", (const char *)val2);
			printf("Expected: %s\n", expr1);
			printf("Which is: %s\n", (const char *)val1);
		} else {
			printf("%s:%d: error: Value of: %s\n", file, line,
			       expr2);
			printf("  Actual: %"PRIdPTR"\n", val2);
			printf("Expected: %s\n", expr1);
			printf("Which is: %"PRIdPTR"\n", val1);
		}
		break;
	case ZUC_OP_NE:
		if (valtype == ZUC_VAL_CSTR) {
			printf("%s:%d: error: ", file, line);
			printf("Expected: (%s) %s (%s), actual: %s == %s\n",
			       expr1, zuc_get_opstr(op), expr2,
			       (char *)val1, (char *)val2);
		} else {
			printf("%s:%d: error: ", file, line);
			printf("Expected: (%s) %s (%s), actual: %"PRIdPTR" vs "
			       "%"PRIdPTR"\n", expr1, zuc_get_opstr(op), expr2,
			       val1, val2);
		}
		break;
	case ZUC_OP_TERMINATE: {
		char const *level = (val1 == 0) ? "error"
			: (val1 == 1) ? "warning"
			: "note";
		printf("%s:%d: %s: %s\n", file, line, level, expr1);
		break;
	}
	case ZUC_OP_TRACEPOINT:
		printf("%s:%d: note: %s\n", file, line, expr1);
		break;
	default:
		printf("%s:%d: error: ", file, line);
		printf("Expected: (%s) %s (%s), actual: %"PRIdPTR" vs "
		       "%"PRIdPTR"\n", expr1, zuc_get_opstr(op), expr2, val1,
		       val2);
	}
}
