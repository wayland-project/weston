/*
 * Copyright © 2014 Pekka Paalanen <pq@iki.fi>
 * Copyright © 2014, 2019 Collabora, Ltd.
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
#include <errno.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include <libweston/libweston.h>
#include <libweston/weston-log.h>
#include "timeline.h"
#include "weston-log-internal.h"

/**
 * Timeline itself is not a subscriber but a scope (a producer of data), and it
 * re-routes the data it produces to all the subscriptions (and implicitly
 * to the subscribers) using a subscription iteration to go through all of them.
 *
 * Public API:
 * * weston_timeline_refresh_subscription_objects() - allows outside parts of
 * libweston notify/signal timeline code about the fact that underlying object
 * has suffered some modifications and needs to re-emit the object ID.
 * * weston_log_timeline_point() -  which will disseminate data to all
 * subscriptions
 *
 * Do note that only weston_timeline_refresh_subscription_objects()
 * is exported in libweston.
 *
 * Destruction of the objects assigned to each underlying objects happens in
 * two places: one in the logging framework callback of the log scope
 * ('destroy_subscription'), and secondly, when the object itself gets
 * destroyed.
 *
 * timeline_emit_context - For each subscription this object will be created to
 * store a buffer when the object itself will be written and a subscription,
 * which will be used to force the object ID if there is a need to do so (the
 * underlying object has been refreshed, or better said has suffered some
 * modification). Data written to a subscription will be flushed before the
 * data written to the FILE *.
 *
 * @param cur a FILE *
 * @param subscription a pointer to an already created subscription
 *
 * @ingroup internal-log
 * @sa weston_timeline_point
 */
struct timeline_emit_context {
	FILE *cur;
	struct weston_log_subscription *subscription;
};

/** Create a timeline subscription and hang it off the subscription
 *
 * Called when the subscription is created.
 *
 * @ingroup internal-log
 */
void
weston_timeline_create_subscription(struct weston_log_subscription *sub,
		void *user_data)
{
	struct weston_timeline_subscription *tl_sub = zalloc(sizeof(*tl_sub));
	if (!tl_sub)
		return;

	wl_list_init(&tl_sub->objects);

	/* attach this timeline_subscription to it */
	weston_log_subscription_set_data(sub, tl_sub);
}

static void
weston_timeline_destroy_subscription_object(struct weston_timeline_subscription_object *sub_obj)
{
	/* remove the notify listener */
	wl_list_remove(&sub_obj->destroy_listener.link);
	sub_obj->destroy_listener.notify = NULL;

	wl_list_remove(&sub_obj->subscription_link);
	free(sub_obj);
}

/** Destroy the timeline subscription and all timeline subscription objects
 * associated with it.
 *
 * Called when (before) the subscription is destroyed.
 *
 * @ingroup internal-log
 */
void
weston_timeline_destroy_subscription(struct weston_log_subscription *sub,
				     void *user_data)
{
	struct weston_timeline_subscription *tl_sub =
		weston_log_subscription_get_data(sub);
	struct weston_timeline_subscription_object *sub_obj, *tmp_sub_obj;

	if (!tl_sub)
		return;

	wl_list_for_each_safe(sub_obj, tmp_sub_obj,
			      &tl_sub->objects, subscription_link)
		weston_timeline_destroy_subscription_object(sub_obj);

	free(tl_sub);
}

static bool
weston_timeline_check_object_refresh(struct weston_timeline_subscription_object *obj)
{
	if (obj->force_refresh == true) {
		obj->force_refresh = false;
		return true;
	}
	return false;
}

static struct weston_timeline_subscription_object *
weston_timeline_subscription_search(struct weston_timeline_subscription *tl_sub,
				    void *object)
{
	struct weston_timeline_subscription_object *sub_obj;

	wl_list_for_each(sub_obj, &tl_sub->objects, subscription_link)
		if (sub_obj->object == object)
			return sub_obj;

	return NULL;
}

static struct weston_timeline_subscription_object *
weston_timeline_subscription_object_create(void *object,
					   struct weston_timeline_subscription *tm_sub)
{
	struct weston_timeline_subscription_object *sub_obj;

	sub_obj = zalloc(sizeof(*sub_obj));
	if (!sub_obj)
		return NULL;

	sub_obj->id = ++tm_sub->next_id;
	sub_obj->object = object;

	/* when the object is created so that it has the chance to display the
	 * object ID, we set the refresh status; it will only be re-freshed by
	 * the backend (or part parts) when the underlying objects has suffered
	 * modifications */
	sub_obj->force_refresh = true;

	wl_list_insert(&tm_sub->objects, &sub_obj->subscription_link);

	return sub_obj;
}

static void
weston_timeline_destroy_subscription_object_notify(struct wl_listener *listener, void *data)
{
	struct weston_timeline_subscription_object *sub_obj;

	sub_obj = wl_container_of(listener, sub_obj, destroy_listener);
	weston_timeline_destroy_subscription_object(sub_obj);
}

static struct weston_timeline_subscription_object *
weston_timeline_subscription_output_ensure(struct weston_timeline_subscription *tl_sub,
		struct weston_output *output)
{
	struct weston_timeline_subscription_object *sub_obj;

	sub_obj = weston_timeline_subscription_search(tl_sub, output);
	if (!sub_obj) {
		sub_obj = weston_timeline_subscription_object_create(output, tl_sub);

		sub_obj->destroy_listener.notify =
			weston_timeline_destroy_subscription_object_notify;
		wl_signal_add(&output->destroy_signal,
			      &sub_obj->destroy_listener);
	}
	return sub_obj;
}

static struct weston_timeline_subscription_object *
weston_timeline_subscription_surface_ensure(struct weston_timeline_subscription *tl_sub,
		struct weston_surface *surface)
{
	struct weston_timeline_subscription_object *sub_obj;

	sub_obj = weston_timeline_subscription_search(tl_sub, surface);
	if (!sub_obj) {
		sub_obj = weston_timeline_subscription_object_create(surface, tl_sub);

		sub_obj->destroy_listener.notify =
			weston_timeline_destroy_subscription_object_notify;
		wl_signal_add(&surface->destroy_signal,
			      &sub_obj->destroy_listener);
	}

	return sub_obj;
}

static void
fprint_quoted_string(struct weston_log_subscription *sub, const char *str)
{
	if (!str) {
		weston_log_subscription_printf(sub, "null");
		return;
	}

	weston_log_subscription_printf(sub, "\"%s\"", str);
}

static void
emit_weston_output_print_id(struct weston_log_subscription *sub,
			    struct weston_timeline_subscription_object *sub_obj,
			    const char *name)
{
	if (!weston_timeline_check_object_refresh(sub_obj))
		return;

	weston_log_subscription_printf(sub, "{ \"id\":%u, "
			"\"type\":\"weston_output\", \"name\":", sub_obj->id);
	fprint_quoted_string(sub, name);
	weston_log_subscription_printf(sub, " }\n");
}

static int
emit_weston_output(struct timeline_emit_context *ctx, void *obj)
{
	struct weston_log_subscription *sub = ctx->subscription;
	struct weston_output *output = obj;
	struct weston_timeline_subscription_object *sub_obj;
	struct weston_timeline_subscription *tl_sub;

	tl_sub = weston_log_subscription_get_data(sub);
	sub_obj = weston_timeline_subscription_output_ensure(tl_sub, output);
	emit_weston_output_print_id(sub, sub_obj, output->name);

	assert(sub_obj->id != 0);
	fprintf(ctx->cur, "\"wo\":%u", sub_obj->id);

	return 1;
}


static void
check_weston_surface_description(struct weston_log_subscription *sub,
				 struct weston_surface *s,
				 struct weston_timeline_subscription *tm_sub,
				 struct weston_timeline_subscription_object *sub_obj)
{
	struct weston_surface *mains;
	char d[512];
	char mainstr[32];

	if (!weston_timeline_check_object_refresh(sub_obj))
		return;

	mains = weston_surface_get_main_surface(s);
	if (mains != s) {
		struct weston_timeline_subscription_object *new_sub_obj;

		new_sub_obj = weston_timeline_subscription_surface_ensure(tm_sub, mains);
		check_weston_surface_description(sub, mains, tm_sub, new_sub_obj);
		if (snprintf(mainstr, sizeof(mainstr), ", \"main_surface\":%u",
			     new_sub_obj->id) < 0)
			mainstr[0] = '\0';
	} else {
		mainstr[0] = '\0';
	}

	if (!s->get_label || s->get_label(s, d, sizeof(d)) < 0)
		d[0] = '\0';

	weston_log_subscription_printf(sub, "{ \"id\":%u, "
				       "\"type\":\"weston_surface\", \"desc\":",
				       sub_obj->id);
	fprint_quoted_string(sub, d[0] ? d : NULL);
	weston_log_subscription_printf(sub, "%s }\n", mainstr);
}

static int
emit_weston_surface(struct timeline_emit_context *ctx, void *obj)
{
	struct weston_log_subscription *sub = ctx->subscription;
	struct weston_surface *surface = obj;
	struct weston_timeline_subscription_object *sub_obj;
	struct weston_timeline_subscription *tl_sub;

	tl_sub = weston_log_subscription_get_data(sub);
	sub_obj = weston_timeline_subscription_surface_ensure(tl_sub, surface);
	check_weston_surface_description(sub, surface, tl_sub, sub_obj);

	assert(sub_obj->id != 0);
	fprintf(ctx->cur, "\"ws\":%u", sub_obj->id);

	return 1;
}

static int
emit_vblank_timestamp(struct timeline_emit_context *ctx, void *obj)
{
	struct timespec *ts = obj;

	fprintf(ctx->cur, "\"vblank_monotonic\":[%" PRId64 ", %ld]",
		(int64_t)ts->tv_sec, ts->tv_nsec);

	return 1;
}

static int
emit_gpu_timestamp(struct timeline_emit_context *ctx, void *obj)
{
	struct timespec *ts = obj;

	fprintf(ctx->cur, "\"gpu\":[%" PRId64 ", %ld]",
		(int64_t)ts->tv_sec, ts->tv_nsec);

	return 1;
}

static struct weston_timeline_subscription_object *
weston_timeline_get_subscription_object(struct weston_log_subscription *sub,
		void *object)
{
	struct weston_timeline_subscription *tl_sub;

	tl_sub = weston_log_subscription_get_data(sub);
	if (!tl_sub)
		return NULL;

	return weston_timeline_subscription_search(tl_sub, object);
}

/** Sets (on) the timeline subscription object refresh status.
 *
 * This function 'notifies' timeline to print the object ID. The timeline code
 * will reset it back, so there's no need for users to do anything about it.
 *
 * Can be used from outside libweston.
 *
 * @param wc a weston_compositor instance
 * @param object the underlying object
 *
 * @ingroup log
 */
WL_EXPORT void
weston_timeline_refresh_subscription_objects(struct weston_compositor *wc,
					     void *object)
{
	struct weston_log_subscription *sub = NULL;

	while ((sub = weston_log_subscription_iterate(wc->timeline, sub))) {
		struct weston_timeline_subscription_object *sub_obj;

		sub_obj = weston_timeline_get_subscription_object(sub, object);
		if (sub_obj)
			sub_obj->force_refresh = true;
	}
}

typedef int (*type_func)(struct timeline_emit_context *ctx, void *obj);

static const type_func type_dispatch[] = {
	[TLT_OUTPUT] = emit_weston_output,
	[TLT_SURFACE] = emit_weston_surface,
	[TLT_VBLANK] = emit_vblank_timestamp,
	[TLT_GPU] = emit_gpu_timestamp,
};

/** Disseminates the message to all subscriptions of the scope \c
 * timeline_scope
 *
 * The TL_POINT() is a wrapper over this function, but it  uses the weston_compositor
 * instance to pass the timeline scope.
 *
 * @param timeline_scope the timeline scope
 * @param name the name of the timeline point. Interpretable by the tool reading
 * the output (wesgr).
 *
 * @ingroup log
 */
WL_EXPORT void
weston_timeline_point(struct weston_log_scope *timeline_scope,
		      const char *name, ...)
{
	struct timespec ts;
	enum timeline_type otype;
	void *obj;
	char buf[512];
	struct weston_log_subscription *sub = NULL;

	if (!weston_log_scope_is_enabled(timeline_scope))
		return;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	while ((sub = weston_log_subscription_iterate(timeline_scope, sub))) {
		va_list argp;
		struct timeline_emit_context ctx = {};

		memset(buf, 0, sizeof(buf));
		ctx.cur = fmemopen(buf, sizeof(buf), "w");
		ctx.subscription = sub;

		if (!ctx.cur) {
			weston_log("Timeline error in fmemopen, closing.\n");
			return;
		}

		fprintf(ctx.cur, "{ \"T\":[%" PRId64 ", %ld], \"N\":\"%s\"",
				(int64_t)ts.tv_sec, ts.tv_nsec, name);

		va_start(argp, name);
		while (1) {
			otype = va_arg(argp, enum timeline_type);
			if (otype == TLT_END)
				break;

			obj = va_arg(argp, void *);
			if (type_dispatch[otype]) {
				fprintf(ctx.cur, ", ");
				type_dispatch[otype](&ctx, obj);
			}
		}
		va_end(argp);

		fprintf(ctx.cur, " }\n");
		fflush(ctx.cur);
		if (ferror(ctx.cur)) {
			weston_log("Timeline error in constructing entry, closing.\n");
		} else {
			weston_log_subscription_printf(ctx.subscription, "%s", buf);
		}

		fclose(ctx.cur);

	}
}
