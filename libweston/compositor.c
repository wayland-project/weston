/*
 * Copyright © 2010-2011 Intel Corporation
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2012-2018 Collabora, Ltd.
 * Copyright © 2017, 2018 General Electric Company
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

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include <linux/input.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>

#include "timeline.h"

#include <libweston/libweston.h>
#include <libweston/weston-log.h>
#include "linux-dmabuf.h"
#include "viewporter-server-protocol.h"
#include "presentation-time-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "linux-explicit-synchronization-unstable-v1-server-protocol.h"
#include "linux-explicit-synchronization.h"
#include "shared/fd-util.h"
#include "shared/helpers.h"
#include "shared/os-compatibility.h"
#include "shared/string-helpers.h"
#include "shared/timespec-util.h"
#include "git-version.h"
#include <libweston/version.h>
#include <libweston/plugin-registry.h>
#include "pixel-formats.h"
#include "backend.h"
#include "libweston-internal.h"

#include "weston-log-internal.h"

/**
 * \defgroup head Head
 * \defgroup output Output
 * \defgroup compositor Compositor
 */

#define DEFAULT_REPAINT_WINDOW 7 /* milliseconds */

static void
weston_output_update_matrix(struct weston_output *output);

static void
weston_output_transform_scale_init(struct weston_output *output,
				   uint32_t transform, uint32_t scale);

static void
weston_compositor_build_view_list(struct weston_compositor *compositor);

static char *
weston_output_create_heads_string(struct weston_output *output);

/** Send wl_output events for mode and scale changes
 *
 * \param head Send on all resources bound to this head.
 * \param mode_changed If true, send the current mode.
 * \param scale_changed If true, send the current scale.
 */
static void
weston_mode_switch_send_events(struct weston_head *head,
			       bool mode_changed, bool scale_changed)
{
	struct weston_output *output = head->output;
	struct wl_resource *resource;
	int version;

	wl_resource_for_each(resource, &head->resource_list) {
		if (mode_changed) {
			wl_output_send_mode(resource,
					    output->current_mode->flags,
					    output->current_mode->width,
					    output->current_mode->height,
					    output->current_mode->refresh);
		}

		version = wl_resource_get_version(resource);
		if (version >= WL_OUTPUT_SCALE_SINCE_VERSION && scale_changed)
			wl_output_send_scale(resource, output->current_scale);

		if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
			wl_output_send_done(resource);
	}
	wl_resource_for_each(resource, &head->xdg_output_resource_list) {
		zxdg_output_v1_send_logical_position(resource,
						     output->x,
						     output->y);
		zxdg_output_v1_send_logical_size(resource,
						 output->width,
						 output->height);
		zxdg_output_v1_send_done(resource);
	}
}

static void
weston_mode_switch_finish(struct weston_output *output,
			  int mode_changed, int scale_changed)
{
	struct weston_seat *seat;
	struct weston_head *head;
	pixman_region32_t old_output_region;

	pixman_region32_init(&old_output_region);
	pixman_region32_copy(&old_output_region, &output->region);

	/* Update output region and transformation matrix */
	weston_output_transform_scale_init(output, output->transform, output->current_scale);

	pixman_region32_init_rect(&output->region, output->x, output->y,
				  output->width, output->height);

	weston_output_update_matrix(output);

	/* If a pointer falls outside the outputs new geometry, move it to its
	 * lower-right corner */
	wl_list_for_each(seat, &output->compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);
		int32_t x, y;

		if (!pointer)
			continue;

		x = wl_fixed_to_int(pointer->x);
		y = wl_fixed_to_int(pointer->y);

		if (!pixman_region32_contains_point(&old_output_region,
						    x, y, NULL) ||
		    pixman_region32_contains_point(&output->region,
						   x, y, NULL))
			continue;

		if (x >= output->x + output->width)
			x = output->x + output->width - 1;
		if (y >= output->y + output->height)
			y = output->y + output->height - 1;

		pointer->x = wl_fixed_from_int(x);
		pointer->y = wl_fixed_from_int(y);
	}

	pixman_region32_fini(&old_output_region);

	if (!mode_changed && !scale_changed)
		return;

	/* notify clients of the changes */
	wl_list_for_each(head, &output->head_list, output_link)
		weston_mode_switch_send_events(head,
					       mode_changed, scale_changed);
}

static void
weston_compositor_reflow_outputs(struct weston_compositor *compositor,
				struct weston_output *resized_output, int delta_width);

/**
 * \ingroup output
 */
WL_EXPORT int
weston_output_mode_set_native(struct weston_output *output,
			      struct weston_mode *mode,
			      int32_t scale)
{
	int ret;
	int mode_changed = 0, scale_changed = 0;
	int32_t old_width;

	if (!output->switch_mode)
		return -1;

	if (!output->original_mode) {
		mode_changed = 1;
		ret = output->switch_mode(output, mode);
		if (ret < 0)
			return ret;
		if (output->current_scale != scale) {
			scale_changed = 1;
			output->current_scale = scale;
		}
	}

	old_width = output->width;
	output->native_mode = mode;
	output->native_scale = scale;

	weston_mode_switch_finish(output, mode_changed, scale_changed);

	if (mode_changed || scale_changed) {
		weston_compositor_reflow_outputs(output->compositor, output, output->width - old_width);

		wl_signal_emit(&output->compositor->output_resized_signal, output);
	}
	return 0;
}

/**
 * \ingroup output
 */
WL_EXPORT int
weston_output_mode_switch_to_native(struct weston_output *output)
{
	int ret;
	int mode_changed = 0, scale_changed = 0;

	if (!output->switch_mode)
		return -1;

	if (!output->original_mode) {
		weston_log("already in the native mode\n");
		return -1;
	}
	/* the non fullscreen clients haven't seen a mode set since we
	 * switched into a temporary, so we need to notify them if the
	 * mode at that time is different from the native mode now.
	 */
	mode_changed = (output->original_mode != output->native_mode);
	scale_changed = (output->original_scale != output->native_scale);

	ret = output->switch_mode(output, output->native_mode);
	if (ret < 0)
		return ret;

	output->current_scale = output->native_scale;

	output->original_mode = NULL;
	output->original_scale = 0;

	weston_mode_switch_finish(output, mode_changed, scale_changed);

	return 0;
}

/**
 * \ingroup output
 */
WL_EXPORT int
weston_output_mode_switch_to_temporary(struct weston_output *output,
				       struct weston_mode *mode,
				       int32_t scale)
{
	int ret;

	if (!output->switch_mode)
		return -1;

	/* original_mode is the last mode non full screen clients have seen,
	 * so we shouldn't change it if we already have one set.
	 */
	if (!output->original_mode) {
		output->original_mode = output->native_mode;
		output->original_scale = output->native_scale;
	}
	ret = output->switch_mode(output, mode);
	if (ret < 0)
		return ret;

	output->current_scale = scale;

	weston_mode_switch_finish(output, 0, 0);

	return 0;
}

static void
region_init_infinite(pixman_region32_t *region)
{
	pixman_region32_init_rect(region, INT32_MIN, INT32_MIN,
				  UINT32_MAX, UINT32_MAX);
}

static struct weston_subsurface *
weston_surface_to_subsurface(struct weston_surface *surface);

WL_EXPORT struct weston_view *
weston_view_create(struct weston_surface *surface)
{
	struct weston_view *view;

	view = zalloc(sizeof *view);
	if (view == NULL)
		return NULL;

	view->surface = surface;
	view->plane = &surface->compositor->primary_plane;

	/* Assign to surface */
	wl_list_insert(&surface->views, &view->surface_link);

	wl_signal_init(&view->destroy_signal);
	wl_list_init(&view->link);
	wl_list_init(&view->layer_link.link);

	pixman_region32_init(&view->clip);

	view->alpha = 1.0;
	pixman_region32_init(&view->transform.opaque);

	wl_list_init(&view->geometry.transformation_list);
	wl_list_insert(&view->geometry.transformation_list,
		       &view->transform.position.link);
	weston_matrix_init(&view->transform.position.matrix);
	wl_list_init(&view->geometry.child_list);
	pixman_region32_init(&view->geometry.scissor);
	pixman_region32_init(&view->transform.boundingbox);
	view->transform.dirty = 1;

	return view;
}

struct weston_frame_callback {
	struct wl_resource *resource;
	struct wl_list link;
};

struct weston_presentation_feedback {
	struct wl_resource *resource;

	/* XXX: could use just wl_resource_get_link() instead */
	struct wl_list link;

	/* The per-surface feedback flags */
	uint32_t psf_flags;
};

static void
weston_presentation_feedback_discard(
		struct weston_presentation_feedback *feedback)
{
	wp_presentation_feedback_send_discarded(feedback->resource);
	wl_resource_destroy(feedback->resource);
}

static void
weston_presentation_feedback_discard_list(struct wl_list *list)
{
	struct weston_presentation_feedback *feedback, *tmp;

	wl_list_for_each_safe(feedback, tmp, list, link)
		weston_presentation_feedback_discard(feedback);
}

static void
weston_presentation_feedback_present(
		struct weston_presentation_feedback *feedback,
		struct weston_output *output,
		uint32_t refresh_nsec,
		const struct timespec *ts,
		uint64_t seq,
		uint32_t flags)
{
	struct wl_client *client = wl_resource_get_client(feedback->resource);
	struct weston_head *head;
	struct wl_resource *o;
	uint32_t tv_sec_hi;
	uint32_t tv_sec_lo;
	uint32_t tv_nsec;
	bool done = false;

	wl_list_for_each(head, &output->head_list, output_link) {
		wl_resource_for_each(o, &head->resource_list) {
			if (wl_resource_get_client(o) != client)
				continue;

			wp_presentation_feedback_send_sync_output(feedback->resource, o);
			done = true;
		}

		/* For clone mode, send it for just one wl_output global,
		 * they are all equivalent anyway.
		 */
		if (done)
			break;
	}

	timespec_to_proto(ts, &tv_sec_hi, &tv_sec_lo, &tv_nsec);
	wp_presentation_feedback_send_presented(feedback->resource,
						tv_sec_hi, tv_sec_lo, tv_nsec,
						refresh_nsec,
						seq >> 32, seq & 0xffffffff,
						flags | feedback->psf_flags);
	wl_resource_destroy(feedback->resource);
}

static void
weston_presentation_feedback_present_list(struct wl_list *list,
					  struct weston_output *output,
					  uint32_t refresh_nsec,
					  const struct timespec *ts,
					  uint64_t seq,
					  uint32_t flags)
{
	struct weston_presentation_feedback *feedback, *tmp;

	assert(!(flags & WP_PRESENTATION_FEEDBACK_INVALID) ||
	       wl_list_empty(list));

	wl_list_for_each_safe(feedback, tmp, list, link)
		weston_presentation_feedback_present(feedback, output,
						     refresh_nsec, ts, seq,
						     flags);
}

static void
surface_state_handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct weston_surface_state *state =
		container_of(listener, struct weston_surface_state,
			     buffer_destroy_listener);

	state->buffer = NULL;
}

static void
weston_surface_state_init(struct weston_surface_state *state)
{
	state->newly_attached = 0;
	state->buffer = NULL;
	state->buffer_destroy_listener.notify =
		surface_state_handle_buffer_destroy;
	state->sx = 0;
	state->sy = 0;

	pixman_region32_init(&state->damage_surface);
	pixman_region32_init(&state->damage_buffer);
	pixman_region32_init(&state->opaque);
	region_init_infinite(&state->input);

	wl_list_init(&state->frame_callback_list);
	wl_list_init(&state->feedback_list);

	state->buffer_viewport.buffer.transform = WL_OUTPUT_TRANSFORM_NORMAL;
	state->buffer_viewport.buffer.scale = 1;
	state->buffer_viewport.buffer.src_width = wl_fixed_from_int(-1);
	state->buffer_viewport.surface.width = -1;
	state->buffer_viewport.changed = 0;

	state->acquire_fence_fd = -1;

	state->desired_protection = WESTON_HDCP_DISABLE;
	state->protection_mode = WESTON_SURFACE_PROTECTION_MODE_RELAXED;
}

static void
weston_surface_state_fini(struct weston_surface_state *state)
{
	struct weston_frame_callback *cb, *next;

	wl_list_for_each_safe(cb, next,
			      &state->frame_callback_list, link)
		wl_resource_destroy(cb->resource);

	weston_presentation_feedback_discard_list(&state->feedback_list);

	pixman_region32_fini(&state->input);
	pixman_region32_fini(&state->opaque);
	pixman_region32_fini(&state->damage_surface);
	pixman_region32_fini(&state->damage_buffer);

	if (state->buffer)
		wl_list_remove(&state->buffer_destroy_listener.link);
	state->buffer = NULL;

	fd_clear(&state->acquire_fence_fd);
	weston_buffer_release_reference(&state->buffer_release_ref, NULL);
}

static void
weston_surface_state_set_buffer(struct weston_surface_state *state,
				struct weston_buffer *buffer)
{
	if (state->buffer == buffer)
		return;

	if (state->buffer)
		wl_list_remove(&state->buffer_destroy_listener.link);
	state->buffer = buffer;
	if (state->buffer)
		wl_signal_add(&state->buffer->destroy_signal,
			      &state->buffer_destroy_listener);
}

WL_EXPORT struct weston_surface *
weston_surface_create(struct weston_compositor *compositor)
{
	struct weston_surface *surface;

	surface = zalloc(sizeof *surface);
	if (surface == NULL)
		return NULL;

	wl_signal_init(&surface->destroy_signal);
	wl_signal_init(&surface->commit_signal);

	surface->compositor = compositor;
	surface->ref_count = 1;

	surface->buffer_viewport.buffer.transform = WL_OUTPUT_TRANSFORM_NORMAL;
	surface->buffer_viewport.buffer.scale = 1;
	surface->buffer_viewport.buffer.src_width = wl_fixed_from_int(-1);
	surface->buffer_viewport.surface.width = -1;

	weston_surface_state_init(&surface->pending);

	pixman_region32_init(&surface->damage);
	pixman_region32_init(&surface->opaque);
	region_init_infinite(&surface->input);

	wl_list_init(&surface->views);

	wl_list_init(&surface->frame_callback_list);
	wl_list_init(&surface->feedback_list);

	wl_list_init(&surface->subsurface_list);
	wl_list_init(&surface->subsurface_list_pending);

	weston_matrix_init(&surface->buffer_to_surface_matrix);
	weston_matrix_init(&surface->surface_to_buffer_matrix);

	wl_list_init(&surface->pointer_constraints);

	surface->acquire_fence_fd = -1;

	surface->desired_protection = WESTON_HDCP_DISABLE;
	surface->current_protection = WESTON_HDCP_DISABLE;
	surface->protection_mode = WESTON_SURFACE_PROTECTION_MODE_RELAXED;

	return surface;
}

WL_EXPORT void
weston_surface_set_color(struct weston_surface *surface,
		 float red, float green, float blue, float alpha)
{
	surface->compositor->renderer->surface_set_color(surface, red, green, blue, alpha);
	surface->is_opaque = !(alpha < 1.0);
}

WL_EXPORT void
weston_view_to_global_float(struct weston_view *view,
			    float sx, float sy, float *x, float *y)
{
	if (view->transform.enabled) {
		struct weston_vector v = { { sx, sy, 0.0f, 1.0f } };

		weston_matrix_transform(&view->transform.matrix, &v);

		if (fabsf(v.f[3]) < 1e-6) {
			weston_log("warning: numerical instability in "
				"%s(), divisor = %g\n", __func__,
				v.f[3]);
			*x = 0;
			*y = 0;
			return;
		}

		*x = v.f[0] / v.f[3];
		*y = v.f[1] / v.f[3];
	} else {
		*x = sx + view->geometry.x;
		*y = sy + view->geometry.y;
	}
}

/** Transform a point to buffer coordinates
 *
 * \param width Surface width.
 * \param height Surface height.
 * \param transform Buffer transform.
 * \param scale Buffer scale.
 * \param sx Surface x coordinate of a point.
 * \param sy Surface y coordinate of a point.
 * \param[out] bx Buffer x coordinate of the point.
 * \param[out] by Buffer Y coordinate of the point.
 *
 * Converts the given surface-local coordinates to buffer coordinates
 * according to the given buffer transform and scale.
 * This ignores wp_viewport.
 *
 * The given width and height must be the result of inverse scaled and
 * inverse transformed buffer size.
 */
WL_EXPORT void
weston_transformed_coord(int width, int height,
			 enum wl_output_transform transform,
			 int32_t scale,
			 float sx, float sy, float *bx, float *by)
{
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
	default:
		*bx = sx;
		*by = sy;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		*bx = width - sx;
		*by = sy;
		break;
	case WL_OUTPUT_TRANSFORM_90:
		*bx = sy;
		*by = width - sx;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		*bx = sy;
		*by = sx;
		break;
	case WL_OUTPUT_TRANSFORM_180:
		*bx = width - sx;
		*by = height - sy;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		*bx = sx;
		*by = height - sy;
		break;
	case WL_OUTPUT_TRANSFORM_270:
		*bx = height - sy;
		*by = sx;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		*bx = height - sy;
		*by = width - sx;
		break;
	}

	*bx *= scale;
	*by *= scale;
}

/** Transform a rectangle to buffer coordinates
 *
 * \param width Surface width.
 * \param height Surface height.
 * \param transform Buffer transform.
 * \param scale Buffer scale.
 * \param rect Rectangle in surface coordinates.
 * \return Rectangle in buffer coordinates.
 *
 * Converts the given surface-local rectangle to buffer coordinates
 * according to the given buffer transform and scale. The resulting
 * rectangle is guaranteed to be well-formed.
 * This ignores wp_viewport.
 *
 * The given width and height must be the result of inverse scaled and
 * inverse transformed buffer size.
 */
WL_EXPORT pixman_box32_t
weston_transformed_rect(int width, int height,
			enum wl_output_transform transform,
			int32_t scale,
			pixman_box32_t rect)
{
	float x1, x2, y1, y2;

	pixman_box32_t ret;

	weston_transformed_coord(width, height, transform, scale,
				 rect.x1, rect.y1, &x1, &y1);
	weston_transformed_coord(width, height, transform, scale,
				 rect.x2, rect.y2, &x2, &y2);

	if (x1 <= x2) {
		ret.x1 = x1;
		ret.x2 = x2;
	} else {
		ret.x1 = x2;
		ret.x2 = x1;
	}

	if (y1 <= y2) {
		ret.y1 = y1;
		ret.y2 = y2;
	} else {
		ret.y1 = y2;
		ret.y2 = y1;
	}

	return ret;
}

/** Transform a region by a matrix, restricted to axis-aligned transformations
 *
 * Warning: This function does not work for projective, affine, or matrices
 * that encode arbitrary rotations. Only 90-degree step rotations are
 * supported.
 */
WL_EXPORT void
weston_matrix_transform_region(pixman_region32_t *dest,
			       struct weston_matrix *matrix,
			       pixman_region32_t *src)
{
	pixman_box32_t *src_rects, *dest_rects;
	int nrects, i;

	src_rects = pixman_region32_rectangles(src, &nrects);
	dest_rects = malloc(nrects * sizeof(*dest_rects));
	if (!dest_rects)
		return;

	for (i = 0; i < nrects; i++) {
		struct weston_vector vec1 = {{
			src_rects[i].x1, src_rects[i].y1, 0, 1
		}};
		weston_matrix_transform(matrix, &vec1);
		vec1.f[0] /= vec1.f[3];
		vec1.f[1] /= vec1.f[3];

		struct weston_vector vec2 = {{
			src_rects[i].x2, src_rects[i].y2, 0, 1
		}};
		weston_matrix_transform(matrix, &vec2);
		vec2.f[0] /= vec2.f[3];
		vec2.f[1] /= vec2.f[3];

		if (vec1.f[0] < vec2.f[0]) {
			dest_rects[i].x1 = floor(vec1.f[0]);
			dest_rects[i].x2 = ceil(vec2.f[0]);
		} else {
			dest_rects[i].x1 = floor(vec2.f[0]);
			dest_rects[i].x2 = ceil(vec1.f[0]);
		}

		if (vec1.f[1] < vec2.f[1]) {
			dest_rects[i].y1 = floor(vec1.f[1]);
			dest_rects[i].y2 = ceil(vec2.f[1]);
		} else {
			dest_rects[i].y1 = floor(vec2.f[1]);
			dest_rects[i].y2 = ceil(vec1.f[1]);
		}
	}

	pixman_region32_clear(dest);
	pixman_region32_init_rects(dest, dest_rects, nrects);
	free(dest_rects);
}

/** Transform a region to buffer coordinates
 *
 * \param width Surface width.
 * \param height Surface height.
 * \param transform Buffer transform.
 * \param scale Buffer scale.
 * \param[in] src Region in surface coordinates.
 * \param[out] dest Resulting region in buffer coordinates.
 *
 * Converts the given surface-local region to buffer coordinates
 * according to the given buffer transform and scale.
 * This ignores wp_viewport.
 *
 * The given width and height must be the result of inverse scaled and
 * inverse transformed buffer size.
 *
 * src and dest are allowed to point to the same memory for in-place conversion.
 */
WL_EXPORT void
weston_transformed_region(int width, int height,
			  enum wl_output_transform transform,
			  int32_t scale,
			  pixman_region32_t *src, pixman_region32_t *dest)
{
	pixman_box32_t *src_rects, *dest_rects;
	int nrects, i;

	if (transform == WL_OUTPUT_TRANSFORM_NORMAL && scale == 1) {
		if (src != dest)
			pixman_region32_copy(dest, src);
		return;
	}

	src_rects = pixman_region32_rectangles(src, &nrects);
	dest_rects = malloc(nrects * sizeof(*dest_rects));
	if (!dest_rects)
		return;

	if (transform == WL_OUTPUT_TRANSFORM_NORMAL) {
		memcpy(dest_rects, src_rects, nrects * sizeof(*dest_rects));
	} else {
		for (i = 0; i < nrects; i++) {
			switch (transform) {
			default:
			case WL_OUTPUT_TRANSFORM_NORMAL:
				dest_rects[i].x1 = src_rects[i].x1;
				dest_rects[i].y1 = src_rects[i].y1;
				dest_rects[i].x2 = src_rects[i].x2;
				dest_rects[i].y2 = src_rects[i].y2;
				break;
			case WL_OUTPUT_TRANSFORM_90:
				dest_rects[i].x1 = src_rects[i].y1;
				dest_rects[i].y1 = width - src_rects[i].x2;
				dest_rects[i].x2 = src_rects[i].y2;
				dest_rects[i].y2 = width - src_rects[i].x1;
				break;
			case WL_OUTPUT_TRANSFORM_180:
				dest_rects[i].x1 = width - src_rects[i].x2;
				dest_rects[i].y1 = height - src_rects[i].y2;
				dest_rects[i].x2 = width - src_rects[i].x1;
				dest_rects[i].y2 = height - src_rects[i].y1;
				break;
			case WL_OUTPUT_TRANSFORM_270:
				dest_rects[i].x1 = height - src_rects[i].y2;
				dest_rects[i].y1 = src_rects[i].x1;
				dest_rects[i].x2 = height - src_rects[i].y1;
				dest_rects[i].y2 = src_rects[i].x2;
				break;
			case WL_OUTPUT_TRANSFORM_FLIPPED:
				dest_rects[i].x1 = width - src_rects[i].x2;
				dest_rects[i].y1 = src_rects[i].y1;
				dest_rects[i].x2 = width - src_rects[i].x1;
				dest_rects[i].y2 = src_rects[i].y2;
				break;
			case WL_OUTPUT_TRANSFORM_FLIPPED_90:
				dest_rects[i].x1 = src_rects[i].y1;
				dest_rects[i].y1 = src_rects[i].x1;
				dest_rects[i].x2 = src_rects[i].y2;
				dest_rects[i].y2 = src_rects[i].x2;
				break;
			case WL_OUTPUT_TRANSFORM_FLIPPED_180:
				dest_rects[i].x1 = src_rects[i].x1;
				dest_rects[i].y1 = height - src_rects[i].y2;
				dest_rects[i].x2 = src_rects[i].x2;
				dest_rects[i].y2 = height - src_rects[i].y1;
				break;
			case WL_OUTPUT_TRANSFORM_FLIPPED_270:
				dest_rects[i].x1 = height - src_rects[i].y2;
				dest_rects[i].y1 = width - src_rects[i].x2;
				dest_rects[i].x2 = height - src_rects[i].y1;
				dest_rects[i].y2 = width - src_rects[i].x1;
				break;
			}
		}
	}

	if (scale != 1) {
		for (i = 0; i < nrects; i++) {
			dest_rects[i].x1 *= scale;
			dest_rects[i].x2 *= scale;
			dest_rects[i].y1 *= scale;
			dest_rects[i].y2 *= scale;
		}
	}

	pixman_region32_clear(dest);
	pixman_region32_init_rects(dest, dest_rects, nrects);
	free(dest_rects);
}

static void
viewport_surface_to_buffer(struct weston_surface *surface,
			   float sx, float sy, float *bx, float *by)
{
	struct weston_buffer_viewport *vp = &surface->buffer_viewport;
	double src_width, src_height;
	double src_x, src_y;

	if (vp->buffer.src_width == wl_fixed_from_int(-1)) {
		if (vp->surface.width == -1) {
			*bx = sx;
			*by = sy;
			return;
		}

		src_x = 0.0;
		src_y = 0.0;
		src_width = surface->width_from_buffer;
		src_height = surface->height_from_buffer;
	} else {
		src_x = wl_fixed_to_double(vp->buffer.src_x);
		src_y = wl_fixed_to_double(vp->buffer.src_y);
		src_width = wl_fixed_to_double(vp->buffer.src_width);
		src_height = wl_fixed_to_double(vp->buffer.src_height);
	}

	*bx = sx * src_width / surface->width + src_x;
	*by = sy * src_height / surface->height + src_y;
}

WL_EXPORT void
weston_surface_to_buffer_float(struct weston_surface *surface,
			       float sx, float sy, float *bx, float *by)
{
	struct weston_buffer_viewport *vp = &surface->buffer_viewport;

	/* first transform coordinates if the viewport is set */
	viewport_surface_to_buffer(surface, sx, sy, bx, by);

	weston_transformed_coord(surface->width_from_buffer,
				 surface->height_from_buffer,
				 vp->buffer.transform, vp->buffer.scale,
				 *bx, *by, bx, by);
}

/** Transform a rectangle from surface coordinates to buffer coordinates
 *
 * \param surface The surface to fetch wp_viewport and buffer transformation
 * from.
 * \param rect The rectangle to transform.
 * \return The transformed rectangle.
 *
 * Viewport and buffer transformations can only do translation, scaling,
 * and rotations in 90-degree steps. Therefore the only loss in the
 * conversion is coordinate rounding.
 *
 * However, some coordinate rounding takes place as an intermediate
 * step before the buffer scale factor is applied, so the rectangle
 * boundary may not be exactly as expected.
 *
 * This is OK for damage tracking since a little extra coverage is
 * not a problem.
 */
WL_EXPORT pixman_box32_t
weston_surface_to_buffer_rect(struct weston_surface *surface,
			      pixman_box32_t rect)
{
	struct weston_buffer_viewport *vp = &surface->buffer_viewport;
	float xf, yf;

	/* first transform box coordinates if the viewport is set */
	viewport_surface_to_buffer(surface, rect.x1, rect.y1, &xf, &yf);
	rect.x1 = floorf(xf);
	rect.y1 = floorf(yf);

	viewport_surface_to_buffer(surface, rect.x2, rect.y2, &xf, &yf);
	rect.x2 = ceilf(xf);
	rect.y2 = ceilf(yf);

	return weston_transformed_rect(surface->width_from_buffer,
				       surface->height_from_buffer,
				       vp->buffer.transform, vp->buffer.scale,
				       rect);
}

/** Transform a region from surface coordinates to buffer coordinates
 *
 * \param surface The surface to fetch wp_viewport and buffer transformation
 * from.
 * \param[in] surface_region The region in surface coordinates.
 * \param[out] buffer_region The region converted to buffer coordinates.
 *
 * Buffer_region must be init'd, but will be completely overwritten.
 *
 * Viewport and buffer transformations can only do translation, scaling,
 * and rotations in 90-degree steps. Therefore the only loss in the
 * conversion is from the coordinate rounding that takes place in
 * \ref weston_surface_to_buffer_rect.
 *
 */
WL_EXPORT void
weston_surface_to_buffer_region(struct weston_surface *surface,
				pixman_region32_t *surface_region,
				pixman_region32_t *buffer_region)
{
	pixman_box32_t *src_rects, *dest_rects;
	int nrects, i;

	src_rects = pixman_region32_rectangles(surface_region, &nrects);
	dest_rects = malloc(nrects * sizeof(*dest_rects));
	if (!dest_rects)
		return;

	for (i = 0; i < nrects; i++) {
		dest_rects[i] = weston_surface_to_buffer_rect(surface,
							      src_rects[i]);
	}

	pixman_region32_fini(buffer_region);
	pixman_region32_init_rects(buffer_region, dest_rects, nrects);
	free(dest_rects);
}

WL_EXPORT void
weston_view_move_to_plane(struct weston_view *view,
			     struct weston_plane *plane)
{
	if (view->plane == plane)
		return;

	weston_view_damage_below(view);
	view->plane = plane;
	weston_surface_damage(view->surface);
}

/** Inflict damage on the plane where the view is visible.
 *
 * \param view The view that causes the damage.
 *
 * If the view is currently on a plane (including the primary plane),
 * take the view's boundingbox, subtract all the opaque views that cover it,
 * and add the remaining region as damage to the plane. This corresponds
 * to the damage inflicted to the plane if this view disappeared.
 *
 * A repaint is scheduled for this view.
 *
 * The region of all opaque views covering this view is stored in
 * weston_view::clip and updated by view_accumulate_damage() during
 * weston_output_repaint(). Specifically, that region matches the
 * scenegraph as it was last painted.
 */
WL_EXPORT void
weston_view_damage_below(struct weston_view *view)
{
	pixman_region32_t damage;

	pixman_region32_init(&damage);
	pixman_region32_subtract(&damage, &view->transform.boundingbox,
				 &view->clip);
	if (view->plane)
		pixman_region32_union(&view->plane->damage,
				      &view->plane->damage, &damage);
	pixman_region32_fini(&damage);
	weston_view_schedule_repaint(view);
}

/** Send wl_surface.enter/leave events
 *
 * \param surface The surface.
 * \param head A head of the entered/left output.
 * \param enter True if entered.
 * \param leave True if left.
 *
 * Send the enter/leave events for all protocol objects bound to the given
 * output by the client owning the surface.
 */
static void
weston_surface_send_enter_leave(struct weston_surface *surface,
				struct weston_head *head,
				bool enter,
				bool leave)
{
	struct wl_resource *wloutput;
	struct wl_client *client;

	assert(enter != leave);

	client = wl_resource_get_client(surface->resource);
	wl_resource_for_each(wloutput, &head->resource_list) {
		if (wl_resource_get_client(wloutput) != client)
			continue;

		if (enter)
			wl_surface_send_enter(surface->resource, wloutput);
		if (leave)
			wl_surface_send_leave(surface->resource, wloutput);
	}
}

static void
weston_surface_compute_protection(struct protected_surface *psurface)
{
	enum weston_hdcp_protection min_protection;
	bool min_protection_valid = false;
	struct weston_surface *surface = psurface->surface;
	struct weston_output *output;

	wl_list_for_each(output, &surface->compositor->output_list, link)
		if (surface->output_mask & (1u << output->id)) {
			/*
			 * If the content-protection is enabled with protection
			 * mode as RELAXED for a surface, and if
			 * content-recording features like: screen-shooter,
			 * recorder, screen-sharing, etc are on, then notify the
			 * client, that the protection is disabled.
			 *
			 * Note: If the protection mode is ENFORCED then there
			 * is no need to bother the client as the renderer takes
			 * care of censoring the visibility of the protected
			 * content.
			 */

			if (output->disable_planes > 0 &&
			    surface->protection_mode == WESTON_SURFACE_PROTECTION_MODE_RELAXED) {
				min_protection = WESTON_HDCP_DISABLE;
				min_protection_valid = true;
				break;
			}
			if (!min_protection_valid) {
				min_protection = output->current_protection;
				min_protection_valid = true;
			}
			if (output->current_protection < min_protection)
				min_protection = output->current_protection;
		}
	if (!min_protection_valid)
		min_protection = WESTON_HDCP_DISABLE;

	surface->current_protection = min_protection;

	weston_protected_surface_send_event(psurface, surface->current_protection);
}

static void
notify_surface_protection_change(void *data)
{
	struct weston_compositor *compositor = data;
	struct content_protection *cp;
	struct protected_surface *psurface;

	cp = compositor->content_protection;
	cp->surface_protection_update = NULL;

	/* Notify the clients, whose surfaces are changed */
	wl_list_for_each(psurface, &cp->protected_list, link)
		if (psurface && psurface->surface)
			weston_surface_compute_protection(psurface);
}

/**
 * \param compositor weston_compositor
 *
 * Schedule an idle task to notify surface about the update in protection,
 * if not already scheduled.
 */
static void
weston_schedule_surface_protection_update(struct weston_compositor *compositor)
{
	struct content_protection *cp = compositor->content_protection;
	struct wl_event_loop *loop;

	if (!cp || cp->surface_protection_update)
		return;
	loop = wl_display_get_event_loop(compositor->wl_display);
	cp->surface_protection_update = wl_event_loop_add_idle(loop,
					       notify_surface_protection_change,
					       compositor);
}

/**
 * \param es    The surface
 * \param mask  The new set of outputs for the surface
 *
 * Sets the surface's set of outputs to the ones specified by
 * the new output mask provided.  Identifies the outputs that
 * have changed, the posts enter and leave events for these
 * outputs as appropriate.
 */
static void
weston_surface_update_output_mask(struct weston_surface *es, uint32_t mask)
{
	uint32_t different = es->output_mask ^ mask;
	uint32_t entered = mask & different;
	uint32_t left = es->output_mask & different;
	uint32_t output_bit;
	struct weston_output *output;
	struct weston_head *head;

	es->output_mask = mask;
	if (es->resource == NULL)
		return;
	if (different == 0)
		return;

	wl_list_for_each(output, &es->compositor->output_list, link) {
		output_bit = 1u << output->id;
		if (!(output_bit & different))
			continue;

		wl_list_for_each(head, &output->head_list, output_link) {
			weston_surface_send_enter_leave(es, head,
							output_bit & entered,
							output_bit & left);
		}
	}
	/*
	 * Change in surfaces' output mask might trigger a change in its
	 * protection.
	 */
	weston_schedule_surface_protection_update(es->compositor);
}

static void
notify_view_output_destroy(struct wl_listener *listener, void *data)
{
	struct weston_view *view =
		container_of(listener,
		     struct weston_view, output_destroy_listener);

	view->output = NULL;
	view->output_destroy_listener.notify = NULL;
}

/** Set the primary output of the view
 *
 * \param view The view whose primary output to set
 * \param output The new primary output for the view
 *
 * Set \a output to be the primary output of the \a view.
 *
 * Notice that the assignment may be temporary; the primary output could be
 * automatically changed. Hence, one cannot rely on the value persisting.
 *
 * Passing NULL as /a output will set the primary output to NULL.
 */
WL_EXPORT void
weston_view_set_output(struct weston_view *view, struct weston_output *output)
{
	if (view->output_destroy_listener.notify) {
		wl_list_remove(&view->output_destroy_listener.link);
		view->output_destroy_listener.notify = NULL;
	}
	view->output = output;
	if (output) {
		view->output_destroy_listener.notify =
			notify_view_output_destroy;
		wl_signal_add(&output->destroy_signal,
			      &view->output_destroy_listener);
	}
}

/** Recalculate which output(s) the surface has views displayed on
 *
 * \param es  The surface to remap to outputs
 *
 * Finds the output that is showing the largest amount of one
 * of the surface's various views.  This output becomes the
 * surface's primary output for vsync and frame callback purposes.
 *
 * Also notes all outputs of all of the surface's views
 * in the output_mask for the surface.
 */
static void
weston_surface_assign_output(struct weston_surface *es)
{
	struct weston_output *new_output;
	struct weston_view *view;
	pixman_region32_t region;
	uint32_t max, area, mask;
	pixman_box32_t *e;

	new_output = NULL;
	max = 0;
	mask = 0;
	pixman_region32_init(&region);
	wl_list_for_each(view, &es->views, surface_link) {
		if (!view->output)
			continue;

		pixman_region32_intersect(&region, &view->transform.boundingbox,
					  &view->output->region);

		e = pixman_region32_extents(&region);
		area = (e->x2 - e->x1) * (e->y2 - e->y1);

		mask |= view->output_mask;

		if (area >= max) {
			new_output = view->output;
			max = area;
		}
	}
	pixman_region32_fini(&region);

	es->output = new_output;
	weston_surface_update_output_mask(es, mask);
}

/** Recalculate which output(s) the view is displayed on
 *
 * \param ev  The view to remap to outputs
 *
 * Identifies the set of outputs that the view is visible on,
 * noting them into the output_mask.  The output that the view
 * is most visible on is set as the view's primary output.
 *
 * Also does the same for the view's surface.  See
 * weston_surface_assign_output().
 */
static void
weston_view_assign_output(struct weston_view *ev)
{
	struct weston_compositor *ec = ev->surface->compositor;
	struct weston_output *output, *new_output;
	pixman_region32_t region;
	uint32_t max, area, mask;
	pixman_box32_t *e;

	new_output = NULL;
	max = 0;
	mask = 0;
	pixman_region32_init(&region);
	wl_list_for_each(output, &ec->output_list, link) {
		if (output->destroying)
			continue;

		pixman_region32_intersect(&region, &ev->transform.boundingbox,
					  &output->region);

		e = pixman_region32_extents(&region);
		area = (e->x2 - e->x1) * (e->y2 - e->y1);

		if (area > 0)
			mask |= 1u << output->id;

		if (area >= max) {
			new_output = output;
			max = area;
		}
	}
	pixman_region32_fini(&region);

	weston_view_set_output(ev, new_output);
	ev->output_mask = mask;

	weston_surface_assign_output(ev->surface);
}

static void
weston_view_to_view_map(struct weston_view *from, struct weston_view *to,
			int from_x, int from_y, int *to_x, int *to_y)
{
	float x, y;

	weston_view_to_global_float(from, from_x, from_y, &x, &y);
	weston_view_from_global_float(to, x, y, &x, &y);

	*to_x = round(x);
	*to_y = round(y);
}

static void
weston_view_transfer_scissor(struct weston_view *from, struct weston_view *to)
{
	pixman_box32_t *a;
	pixman_box32_t b;

	a = pixman_region32_extents(&from->geometry.scissor);

	weston_view_to_view_map(from, to, a->x1, a->y1, &b.x1, &b.y1);
	weston_view_to_view_map(from, to, a->x2, a->y2, &b.x2, &b.y2);

	pixman_region32_fini(&to->geometry.scissor);
	pixman_region32_init_with_extents(&to->geometry.scissor, &b);
}

static void
view_compute_bbox(struct weston_view *view, const pixman_box32_t *inbox,
		  pixman_region32_t *bbox)
{
	float min_x = HUGE_VALF,  min_y = HUGE_VALF;
	float max_x = -HUGE_VALF, max_y = -HUGE_VALF;
	int32_t s[4][2] = {
		{ inbox->x1, inbox->y1 },
		{ inbox->x1, inbox->y2 },
		{ inbox->x2, inbox->y1 },
		{ inbox->x2, inbox->y2 },
	};
	float int_x, int_y;
	int i;

	if (inbox->x1 == inbox->x2 || inbox->y1 == inbox->y2) {
		/* avoid rounding empty bbox to 1x1 */
		pixman_region32_init(bbox);
		return;
	}

	for (i = 0; i < 4; ++i) {
		float x, y;
		weston_view_to_global_float(view, s[i][0], s[i][1], &x, &y);
		if (x < min_x)
			min_x = x;
		if (x > max_x)
			max_x = x;
		if (y < min_y)
			min_y = y;
		if (y > max_y)
			max_y = y;
	}

	int_x = floorf(min_x);
	int_y = floorf(min_y);
	pixman_region32_init_rect(bbox, int_x, int_y,
				  ceilf(max_x) - int_x, ceilf(max_y) - int_y);
}

static void
weston_view_update_transform_scissor(struct weston_view *view,
				     pixman_region32_t *region)
{
	struct weston_view *parent = view->geometry.parent;

	if (parent) {
		if (parent->geometry.scissor_enabled) {
			view->geometry.scissor_enabled = true;
			weston_view_transfer_scissor(parent, view);
		} else {
			view->geometry.scissor_enabled = false;
		}
	}

	if (view->geometry.scissor_enabled)
		pixman_region32_intersect(region, region,
					  &view->geometry.scissor);
}
static void
weston_view_update_transform_disable(struct weston_view *view)
{
	view->transform.enabled = 0;

	/* round off fractions when not transformed */
	view->geometry.x = roundf(view->geometry.x);
	view->geometry.y = roundf(view->geometry.y);

	/* Otherwise identity matrix, but with x and y translation. */
	view->transform.position.matrix.type = WESTON_MATRIX_TRANSFORM_TRANSLATE;
	view->transform.position.matrix.d[12] = view->geometry.x;
	view->transform.position.matrix.d[13] = view->geometry.y;

	view->transform.matrix = view->transform.position.matrix;

	view->transform.inverse = view->transform.position.matrix;
	view->transform.inverse.d[12] = -view->geometry.x;
	view->transform.inverse.d[13] = -view->geometry.y;

	pixman_region32_init_rect(&view->transform.boundingbox,
				  0, 0,
				  view->surface->width,
				  view->surface->height);

	weston_view_update_transform_scissor(view, &view->transform.boundingbox);

	pixman_region32_translate(&view->transform.boundingbox,
				  view->geometry.x, view->geometry.y);

	if (view->alpha == 1.0) {
		pixman_region32_copy(&view->transform.opaque,
				     &view->surface->opaque);
		pixman_region32_intersect(&view->transform.opaque,
					  &view->transform.opaque,
					  &view->geometry.scissor);
		pixman_region32_translate(&view->transform.opaque,
					  view->geometry.x,
					  view->geometry.y);
	}
}

static int
weston_view_update_transform_enable(struct weston_view *view)
{
	struct weston_view *parent = view->geometry.parent;
	struct weston_matrix *matrix = &view->transform.matrix;
	struct weston_matrix *inverse = &view->transform.inverse;
	struct weston_transform *tform;
	pixman_region32_t surfregion;
	const pixman_box32_t *surfbox;

	view->transform.enabled = 1;

	/* Otherwise identity matrix, but with x and y translation. */
	view->transform.position.matrix.type = WESTON_MATRIX_TRANSFORM_TRANSLATE;
	view->transform.position.matrix.d[12] = view->geometry.x;
	view->transform.position.matrix.d[13] = view->geometry.y;

	weston_matrix_init(matrix);
	wl_list_for_each(tform, &view->geometry.transformation_list, link)
		weston_matrix_multiply(matrix, &tform->matrix);

	if (parent)
		weston_matrix_multiply(matrix, &parent->transform.matrix);

	if (weston_matrix_invert(inverse, matrix) < 0) {
		/* Oops, bad total transformation, not invertible */
		weston_log("error: weston_view %p"
			" transformation not invertible.\n", view);
		return -1;
	}

	pixman_region32_init_rect(&surfregion, 0, 0,
				  view->surface->width, view->surface->height);

	weston_view_update_transform_scissor(view, &surfregion);

	surfbox = pixman_region32_extents(&surfregion);

	view_compute_bbox(view, surfbox, &view->transform.boundingbox);
	pixman_region32_fini(&surfregion);

	if (view->alpha == 1.0 &&
	    matrix->type == WESTON_MATRIX_TRANSFORM_TRANSLATE) {
		pixman_region32_copy(&view->transform.opaque,
				     &view->surface->opaque);
		pixman_region32_intersect(&view->transform.opaque,
					  &view->transform.opaque,
					  &view->geometry.scissor);
		pixman_region32_translate(&view->transform.opaque,
					  matrix->d[12],
					  matrix->d[13]);
	}

	return 0;
}

static struct weston_layer *
get_view_layer(struct weston_view *view)
{
	if (view->parent_view)
		return get_view_layer(view->parent_view);
	return view->layer_link.layer;
}

WL_EXPORT void
weston_view_update_transform(struct weston_view *view)
{
	struct weston_view *parent = view->geometry.parent;
	struct weston_layer *layer;
	pixman_region32_t mask;

	if (!view->transform.dirty)
		return;

	if (parent)
		weston_view_update_transform(parent);

	view->transform.dirty = 0;

	weston_view_damage_below(view);

	pixman_region32_fini(&view->transform.boundingbox);
	pixman_region32_fini(&view->transform.opaque);
	pixman_region32_init(&view->transform.opaque);

	/* transform.position is always in transformation_list */
	if (view->geometry.transformation_list.next ==
	    &view->transform.position.link &&
	    view->geometry.transformation_list.prev ==
	    &view->transform.position.link &&
	    !parent) {
		weston_view_update_transform_disable(view);
	} else {
		if (weston_view_update_transform_enable(view) < 0)
			weston_view_update_transform_disable(view);
	}

	layer = get_view_layer(view);
	if (layer) {
		pixman_region32_init_with_extents(&mask, &layer->mask);
		pixman_region32_intersect(&view->transform.boundingbox,
					  &view->transform.boundingbox, &mask);
		pixman_region32_intersect(&view->transform.opaque,
					  &view->transform.opaque, &mask);
		pixman_region32_fini(&mask);
	}

	weston_view_damage_below(view);

	weston_view_assign_output(view);

	wl_signal_emit(&view->surface->compositor->transform_signal,
		       view->surface);
}

WL_EXPORT void
weston_view_geometry_dirty(struct weston_view *view)
{
	struct weston_view *child;

	/*
	 * The invariant: if view->geometry.dirty, then all views
	 * in view->geometry.child_list have geometry.dirty too.
	 * Corollary: if not parent->geometry.dirty, then all ancestors
	 * are not dirty.
	 */

	if (view->transform.dirty)
		return;

	view->transform.dirty = 1;

	wl_list_for_each(child, &view->geometry.child_list,
			 geometry.parent_link)
		weston_view_geometry_dirty(child);
}

WL_EXPORT void
weston_view_to_global_fixed(struct weston_view *view,
			    wl_fixed_t vx, wl_fixed_t vy,
			    wl_fixed_t *x, wl_fixed_t *y)
{
	float xf, yf;

	weston_view_to_global_float(view,
				    wl_fixed_to_double(vx),
				    wl_fixed_to_double(vy),
				    &xf, &yf);
	*x = wl_fixed_from_double(xf);
	*y = wl_fixed_from_double(yf);
}

WL_EXPORT void
weston_view_from_global_float(struct weston_view *view,
			      float x, float y, float *vx, float *vy)
{
	if (view->transform.enabled) {
		struct weston_vector v = { { x, y, 0.0f, 1.0f } };

		weston_matrix_transform(&view->transform.inverse, &v);

		if (fabsf(v.f[3]) < 1e-6) {
			weston_log("warning: numerical instability in "
				"weston_view_from_global(), divisor = %g\n",
				v.f[3]);
			*vx = 0;
			*vy = 0;
			return;
		}

		*vx = v.f[0] / v.f[3];
		*vy = v.f[1] / v.f[3];
	} else {
		*vx = x - view->geometry.x;
		*vy = y - view->geometry.y;
	}
}

WL_EXPORT void
weston_view_from_global_fixed(struct weston_view *view,
			      wl_fixed_t x, wl_fixed_t y,
			      wl_fixed_t *vx, wl_fixed_t *vy)
{
	float vxf, vyf;

	weston_view_from_global_float(view,
				      wl_fixed_to_double(x),
				      wl_fixed_to_double(y),
				      &vxf, &vyf);
	*vx = wl_fixed_from_double(vxf);
	*vy = wl_fixed_from_double(vyf);
}

WL_EXPORT void
weston_view_from_global(struct weston_view *view,
			int32_t x, int32_t y, int32_t *vx, int32_t *vy)
{
	float vxf, vyf;

	weston_view_from_global_float(view, x, y, &vxf, &vyf);
	*vx = floorf(vxf);
	*vy = floorf(vyf);
}

/**
 * \param surface  The surface to be repainted
 *
 * Marks the output(s) that the surface is shown on as needing to be
 * repainted.  See weston_output_schedule_repaint().
 */
WL_EXPORT void
weston_surface_schedule_repaint(struct weston_surface *surface)
{
	struct weston_output *output;

	wl_list_for_each(output, &surface->compositor->output_list, link)
		if (surface->output_mask & (1u << output->id))
			weston_output_schedule_repaint(output);
}

/**
 * \param view  The view to be repainted
 *
 * Marks the output(s) that the view is shown on as needing to be
 * repainted.  See weston_output_schedule_repaint().
 */
WL_EXPORT void
weston_view_schedule_repaint(struct weston_view *view)
{
	struct weston_output *output;

	wl_list_for_each(output, &view->surface->compositor->output_list, link)
		if (view->output_mask & (1u << output->id))
			weston_output_schedule_repaint(output);
}

/**
 * XXX: This function does it the wrong way.
 * surface->damage is the damage from the client, and causes
 * surface_flush_damage() to copy pixels. No window management action can
 * cause damage to the client-provided content, warranting re-upload!
 *
 * Instead of surface->damage, this function should record the damage
 * with all the views for this surface to avoid extraneous texture
 * uploads.
 */
WL_EXPORT void
weston_surface_damage(struct weston_surface *surface)
{
	pixman_region32_union_rect(&surface->damage, &surface->damage,
				   0, 0, surface->width,
				   surface->height);

	weston_surface_schedule_repaint(surface);
}

WL_EXPORT void
weston_view_set_position(struct weston_view *view, float x, float y)
{
	if (view->geometry.x == x && view->geometry.y == y)
		return;

	view->geometry.x = x;
	view->geometry.y = y;
	weston_view_geometry_dirty(view);
}

static void
transform_parent_handle_parent_destroy(struct wl_listener *listener,
				       void *data)
{
	struct weston_view *view =
		container_of(listener, struct weston_view,
			     geometry.parent_destroy_listener);

	weston_view_set_transform_parent(view, NULL);
}

WL_EXPORT void
weston_view_set_transform_parent(struct weston_view *view,
				 struct weston_view *parent)
{
	if (view->geometry.parent) {
		wl_list_remove(&view->geometry.parent_destroy_listener.link);
		wl_list_remove(&view->geometry.parent_link);

		if (!parent)
			view->geometry.scissor_enabled = false;
	}

	view->geometry.parent = parent;

	view->geometry.parent_destroy_listener.notify =
		transform_parent_handle_parent_destroy;
	if (parent) {
		wl_signal_add(&parent->destroy_signal,
			      &view->geometry.parent_destroy_listener);
		wl_list_insert(&parent->geometry.child_list,
			       &view->geometry.parent_link);
	}

	weston_view_geometry_dirty(view);
}

/** Set a clip mask rectangle on a view
 *
 * \param view The view to set the clip mask on.
 * \param x Top-left corner X coordinate of the clip rectangle.
 * \param y Top-left corner Y coordinate of the clip rectangle.
 * \param width Width of the clip rectangle, non-negative.
 * \param height Height of the clip rectangle, non-negative.
 *
 * A shell may set a clip mask rectangle on a view. Everything outside
 * the rectangle is cut away for input and output purposes: it is
 * not drawn and cannot be hit by hit-test based input like pointer
 * motion or touch-downs. Everything inside the rectangle will behave
 * normally. Clients are unaware of clipping.
 *
 * The rectangle is set in surface-local coordinates. Setting a clip
 * mask rectangle does not affect the view position, the view is positioned
 * as it would be without a clip. The clip also does not change
 * weston_surface::width,height.
 *
 * The clip mask rectangle is part of transformation inheritance
 * (weston_view_set_transform_parent()). A clip set in the root of the
 * transformation inheritance tree will affect all views in the tree.
 * A clip can be set only on the root view. Attempting to set a clip
 * on view that has a transformation parent will fail. Assigning a parent
 * to a view that has a clip set will cause the clip to be forgotten.
 *
 * Because the clip mask is an axis-aligned rectangle, it poses restrictions
 * on the additional transformations in the child views. These transformations
 * may not rotate the coordinate axes, i.e., only translation and scaling
 * are allowed. Violating this restriction causes the clipping to malfunction.
 * Furthermore, using scaling may cause rounding errors in child clipping.
 *
 * The clip mask rectangle is not automatically adjusted based on
 * wl_surface.attach dx and dy arguments.
 *
 * A clip mask rectangle can be set only if the compositor capability
 * WESTON_CAP_VIEW_CLIP_MASK is present.
 *
 * This function sets the clip mask rectangle and schedules a repaint for
 * the view.
 */
WL_EXPORT void
weston_view_set_mask(struct weston_view *view,
		     int x, int y, int width, int height)
{
	struct weston_compositor *compositor = view->surface->compositor;

	if (!(compositor->capabilities & WESTON_CAP_VIEW_CLIP_MASK)) {
		weston_log("%s not allowed without capability!\n", __func__);
		return;
	}

	if (view->geometry.parent) {
		weston_log("view %p has a parent, clip forbidden!\n", view);
		return;
	}

	if (width < 0 || height < 0) {
		weston_log("%s: illegal args %d, %d, %d, %d\n", __func__,
			   x, y, width, height);
		return;
	}

	pixman_region32_fini(&view->geometry.scissor);
	pixman_region32_init_rect(&view->geometry.scissor, x, y, width, height);
	view->geometry.scissor_enabled = true;
	weston_view_geometry_dirty(view);
	weston_view_schedule_repaint(view);
}

/** Remove the clip mask from a view
 *
 * \param view The view to remove the clip mask from.
 *
 * Removed the clip mask rectangle and schedules a repaint.
 *
 * \sa weston_view_set_mask
 */
WL_EXPORT void
weston_view_set_mask_infinite(struct weston_view *view)
{
	view->geometry.scissor_enabled = false;
	weston_view_geometry_dirty(view);
	weston_view_schedule_repaint(view);
}

/* Check if view should be displayed
 *
 * The indicator is set manually when assigning
 * a view to a surface.
 *
 * This needs reworking. See the thread starting at:
 *
 * https://lists.freedesktop.org/archives/wayland-devel/2016-June/029656.html
 */
WL_EXPORT bool
weston_view_is_mapped(struct weston_view *view)
{
	return view->is_mapped;
}

/* Check if view is opaque in specified region
 *
 * \param view The view to check for opacity.
 * \param region The region to check for opacity, in view coordinates.
 *
 * Returns true if the view is opaque in the specified region, because view
 * alpha is 1.0 and either the opaque region set by the client contains the
 * specified region, or the buffer pixel format or solid color is opaque.
 */
WL_EXPORT bool
weston_view_is_opaque(struct weston_view *ev, pixman_region32_t *region)
{
	pixman_region32_t r;
	bool ret = false;

	if (ev->alpha < 1.0)
		return false;

	if (ev->surface->is_opaque)
		return true;

	if (ev->transform.dirty)
		return false;

	pixman_region32_init(&r);
	pixman_region32_subtract(&r, region, &ev->transform.opaque);

	if (!pixman_region32_not_empty(&r))
		ret = true;

	pixman_region32_fini(&r);

	return ret;
}

/** Check if the view has a valid buffer available
 *
 * @param ev The view to check if it has a valid buffer.
 *
 * Returns true if the view has a valid buffer or false otherwise.
 */
WL_EXPORT bool
weston_view_has_valid_buffer(struct weston_view *ev)
{
	return ev->surface->buffer_ref.buffer != NULL;
}

/** Check if the view matches the entire output
 *
 * @param ev The view to check.
 * @param output The output to check against.
 *
 * Returns true if the view does indeed matches the entire output.
 */
WL_EXPORT bool
weston_view_matches_output_entirely(struct weston_view *ev,
				    struct weston_output *output)
{
	pixman_box32_t *extents =
		pixman_region32_extents(&ev->transform.boundingbox);

	if (extents->x1 != output->x ||
	    extents->y1 != output->y ||
	    extents->x2 != output->x + output->width ||
	    extents->y2 != output->y + output->height)
		return false;

	return true;
}

/* Check if a surface has a view assigned to it
 *
 * The indicator is set manually when mapping
 * a surface and creating a view for it.
 *
 * This needs to go. See the thread starting at:
 *
 * https://lists.freedesktop.org/archives/wayland-devel/2016-June/029656.html
 *
 */
WL_EXPORT bool
weston_surface_is_mapped(struct weston_surface *surface)
{
	return surface->is_mapped;
}

static void
surface_set_size(struct weston_surface *surface, int32_t width, int32_t height)
{
	struct weston_view *view;

	if (surface->width == width && surface->height == height)
		return;

	surface->width = width;
	surface->height = height;

	wl_list_for_each(view, &surface->views, surface_link)
		weston_view_geometry_dirty(view);
}

WL_EXPORT void
weston_surface_set_size(struct weston_surface *surface,
			int32_t width, int32_t height)
{
	assert(!surface->resource);
	surface_set_size(surface, width, height);
}

static int
fixed_round_up_to_int(wl_fixed_t f)
{
	return wl_fixed_to_int(wl_fixed_from_int(1) - 1 + f);
}

static void
convert_size_by_transform_scale(int32_t *width_out, int32_t *height_out,
				int32_t width, int32_t height,
				uint32_t transform,
				int32_t scale)
{
	assert(scale > 0);

	switch (transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		*width_out = width / scale;
		*height_out = height / scale;
		break;
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		*width_out = height / scale;
		*height_out = width / scale;
		break;
	default:
		assert(0 && "invalid transform");
	}
}

static void
weston_surface_calculate_size_from_buffer(struct weston_surface *surface)
{
	struct weston_buffer_viewport *vp = &surface->buffer_viewport;

	if (!surface->buffer_ref.buffer) {
		surface->width_from_buffer = 0;
		surface->height_from_buffer = 0;
		return;
	}

	convert_size_by_transform_scale(&surface->width_from_buffer,
					&surface->height_from_buffer,
					surface->buffer_ref.buffer->width,
					surface->buffer_ref.buffer->height,
					vp->buffer.transform,
					vp->buffer.scale);
}

static void
weston_surface_update_size(struct weston_surface *surface)
{
	struct weston_buffer_viewport *vp = &surface->buffer_viewport;
	int32_t width, height;

	width = surface->width_from_buffer;
	height = surface->height_from_buffer;

	if (width != 0 && vp->surface.width != -1) {
		surface_set_size(surface,
				 vp->surface.width, vp->surface.height);
		return;
	}

	if (width != 0 && vp->buffer.src_width != wl_fixed_from_int(-1)) {
		int32_t w = fixed_round_up_to_int(vp->buffer.src_width);
		int32_t h = fixed_round_up_to_int(vp->buffer.src_height);

		surface_set_size(surface, w ?: 1, h ?: 1);
		return;
	}

	surface_set_size(surface, width, height);
}

/** weston_compositor_get_time
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_get_time(struct timespec *time)
{
	clock_gettime(CLOCK_REALTIME, time);
}

/** weston_compositor_pick_view
 * \ingroup compositor
 */
WL_EXPORT struct weston_view *
weston_compositor_pick_view(struct weston_compositor *compositor,
			    wl_fixed_t x, wl_fixed_t y,
			    wl_fixed_t *vx, wl_fixed_t *vy)
{
	struct weston_view *view;
	wl_fixed_t view_x, view_y;
	int view_ix, view_iy;
	int ix = wl_fixed_to_int(x);
	int iy = wl_fixed_to_int(y);

	wl_list_for_each(view, &compositor->view_list, link) {
		if (!pixman_region32_contains_point(
				&view->transform.boundingbox, ix, iy, NULL))
			continue;

		weston_view_from_global_fixed(view, x, y, &view_x, &view_y);
		view_ix = wl_fixed_to_int(view_x);
		view_iy = wl_fixed_to_int(view_y);

		if (!pixman_region32_contains_point(&view->surface->input,
						    view_ix, view_iy, NULL))
			continue;

		if (view->geometry.scissor_enabled &&
		    !pixman_region32_contains_point(&view->geometry.scissor,
						    view_ix, view_iy, NULL))
			continue;

		*vx = view_x;
		*vy = view_y;
		return view;
	}

	*vx = wl_fixed_from_int(-1000000);
	*vy = wl_fixed_from_int(-1000000);
	return NULL;
}

static void
weston_compositor_repick(struct weston_compositor *compositor)
{
	struct weston_seat *seat;

	if (!compositor->session_active)
		return;

	wl_list_for_each(seat, &compositor->seat_list, link)
		weston_seat_repick(seat);
}

WL_EXPORT void
weston_view_unmap(struct weston_view *view)
{
	struct weston_seat *seat;

	if (!weston_view_is_mapped(view))
		return;

	weston_view_damage_below(view);
	weston_view_set_output(view, NULL);
	view->plane = NULL;
	view->is_mapped = false;
	weston_layer_entry_remove(&view->layer_link);
	wl_list_remove(&view->link);
	wl_list_init(&view->link);
	view->output_mask = 0;
	weston_surface_assign_output(view->surface);

	if (weston_surface_is_mapped(view->surface))
		return;

	wl_list_for_each(seat, &view->surface->compositor->seat_list, link) {
		struct weston_touch *touch = weston_seat_get_touch(seat);
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);
		struct weston_keyboard *keyboard =
			weston_seat_get_keyboard(seat);

		if (keyboard && keyboard->focus == view->surface)
			weston_keyboard_set_focus(keyboard, NULL);
		if (pointer && pointer->focus == view)
			weston_pointer_clear_focus(pointer);
		if (touch && touch->focus == view)
			weston_touch_set_focus(touch, NULL);
	}
}

WL_EXPORT void
weston_surface_unmap(struct weston_surface *surface)
{
	struct weston_view *view;

	surface->is_mapped = false;
	wl_list_for_each(view, &surface->views, surface_link)
		weston_view_unmap(view);
	surface->output = NULL;
}

static void
weston_surface_reset_pending_buffer(struct weston_surface *surface)
{
	weston_surface_state_set_buffer(&surface->pending, NULL);
	surface->pending.sx = 0;
	surface->pending.sy = 0;
	surface->pending.newly_attached = 0;
	surface->pending.buffer_viewport.changed = 0;
}

WL_EXPORT void
weston_view_destroy(struct weston_view *view)
{
	wl_signal_emit(&view->destroy_signal, view);

	assert(wl_list_empty(&view->geometry.child_list));

	if (weston_view_is_mapped(view)) {
		weston_view_unmap(view);
		weston_compositor_build_view_list(view->surface->compositor);
	}

	wl_list_remove(&view->link);
	weston_layer_entry_remove(&view->layer_link);

	pixman_region32_fini(&view->clip);
	pixman_region32_fini(&view->geometry.scissor);
	pixman_region32_fini(&view->transform.boundingbox);
	pixman_region32_fini(&view->transform.opaque);

	weston_view_set_transform_parent(view, NULL);
	weston_view_set_output(view, NULL);

	wl_list_remove(&view->surface_link);

	free(view);
}

WL_EXPORT void
weston_surface_destroy(struct weston_surface *surface)
{
	struct weston_frame_callback *cb, *next;
	struct weston_view *ev, *nv;
	struct weston_pointer_constraint *constraint, *next_constraint;

	if (--surface->ref_count > 0)
		return;

	assert(surface->resource == NULL);

	wl_signal_emit(&surface->destroy_signal, surface);

	assert(wl_list_empty(&surface->subsurface_list_pending));
	assert(wl_list_empty(&surface->subsurface_list));

	wl_list_for_each_safe(ev, nv, &surface->views, surface_link)
		weston_view_destroy(ev);

	weston_surface_state_fini(&surface->pending);

	weston_buffer_reference(&surface->buffer_ref, NULL);
	weston_buffer_release_reference(&surface->buffer_release_ref, NULL);

	pixman_region32_fini(&surface->damage);
	pixman_region32_fini(&surface->opaque);
	pixman_region32_fini(&surface->input);

	wl_list_for_each_safe(cb, next, &surface->frame_callback_list, link)
		wl_resource_destroy(cb->resource);

	weston_presentation_feedback_discard_list(&surface->feedback_list);

	wl_list_for_each_safe(constraint, next_constraint,
			      &surface->pointer_constraints,
			      link)
		weston_pointer_constraint_destroy(constraint);

	fd_clear(&surface->acquire_fence_fd);

	free(surface);
}

static void
destroy_surface(struct wl_resource *resource)
{
	struct weston_surface *surface = wl_resource_get_user_data(resource);

	assert(surface);

	/* Set the resource to NULL, since we don't want to leave a
	 * dangling pointer if the surface was refcounted and survives
	 * the weston_surface_destroy() call. */
	surface->resource = NULL;

	if (surface->viewport_resource)
		wl_resource_set_user_data(surface->viewport_resource, NULL);

	if (surface->synchronization_resource) {
		wl_resource_set_user_data(surface->synchronization_resource,
					  NULL);
	}

	weston_surface_destroy(surface);
}

static void
weston_buffer_destroy_handler(struct wl_listener *listener, void *data)
{
	struct weston_buffer *buffer =
		container_of(listener, struct weston_buffer, destroy_listener);

	wl_signal_emit(&buffer->destroy_signal, buffer);
	free(buffer);
}

WL_EXPORT struct weston_buffer *
weston_buffer_from_resource(struct wl_resource *resource)
{
	struct weston_buffer *buffer;
	struct wl_listener *listener;

	listener = wl_resource_get_destroy_listener(resource,
						    weston_buffer_destroy_handler);

	if (listener)
		return container_of(listener, struct weston_buffer,
				    destroy_listener);

	buffer = zalloc(sizeof *buffer);
	if (buffer == NULL)
		return NULL;

	buffer->resource = resource;
	wl_signal_init(&buffer->destroy_signal);
	buffer->destroy_listener.notify = weston_buffer_destroy_handler;
	buffer->y_inverted = 1;
	wl_resource_add_destroy_listener(resource, &buffer->destroy_listener);

	return buffer;
}

static void
weston_buffer_reference_handle_destroy(struct wl_listener *listener,
				       void *data)
{
	struct weston_buffer_reference *ref =
		container_of(listener, struct weston_buffer_reference,
			     destroy_listener);

	assert((struct weston_buffer *)data == ref->buffer);
	ref->buffer = NULL;
}

WL_EXPORT void
weston_buffer_reference(struct weston_buffer_reference *ref,
			struct weston_buffer *buffer)
{
	if (ref->buffer && buffer != ref->buffer) {
		ref->buffer->busy_count--;
		if (ref->buffer->busy_count == 0) {
			assert(wl_resource_get_client(ref->buffer->resource));
			wl_buffer_send_release(ref->buffer->resource);
		}
		wl_list_remove(&ref->destroy_listener.link);
	}

	if (buffer && buffer != ref->buffer) {
		buffer->busy_count++;
		wl_signal_add(&buffer->destroy_signal,
			      &ref->destroy_listener);
	}

	ref->buffer = buffer;
	ref->destroy_listener.notify = weston_buffer_reference_handle_destroy;
}

static void
weston_buffer_release_reference_handle_destroy(struct wl_listener *listener,
					       void *data)
{
	struct weston_buffer_release_reference *ref =
		container_of(listener, struct weston_buffer_release_reference,
			     destroy_listener);

	assert((struct wl_resource *)data == ref->buffer_release->resource);
	ref->buffer_release = NULL;
}

static void
weston_buffer_release_destroy(struct weston_buffer_release *buffer_release)
{
	struct wl_resource *resource = buffer_release->resource;
	int release_fence_fd = buffer_release->fence_fd;

	if (release_fence_fd >= 0) {
		zwp_linux_buffer_release_v1_send_fenced_release(
			resource, release_fence_fd);
	} else {
		zwp_linux_buffer_release_v1_send_immediate_release(
			resource);
	}

	wl_resource_destroy(resource);
}

WL_EXPORT void
weston_buffer_release_reference(struct weston_buffer_release_reference *ref,
				struct weston_buffer_release *buffer_release)
{
	if (buffer_release == ref->buffer_release)
		return;

	if (ref->buffer_release) {
		ref->buffer_release->ref_count--;
		wl_list_remove(&ref->destroy_listener.link);
		if (ref->buffer_release->ref_count == 0)
			weston_buffer_release_destroy(ref->buffer_release);
	}

	if (buffer_release) {
		buffer_release->ref_count++;
		wl_resource_add_destroy_listener(buffer_release->resource,
						 &ref->destroy_listener);
	}

	ref->buffer_release = buffer_release;
	ref->destroy_listener.notify =
		weston_buffer_release_reference_handle_destroy;
}

WL_EXPORT void
weston_buffer_release_move(struct weston_buffer_release_reference *dest,
			   struct weston_buffer_release_reference *src)
{
	weston_buffer_release_reference(dest, src->buffer_release);
	weston_buffer_release_reference(src, NULL);
}

static void
weston_surface_attach(struct weston_surface *surface,
		      struct weston_buffer *buffer)
{
	weston_buffer_reference(&surface->buffer_ref, buffer);

	if (!buffer) {
		if (weston_surface_is_mapped(surface))
			weston_surface_unmap(surface);
	}

	surface->compositor->renderer->attach(surface, buffer);

	weston_surface_calculate_size_from_buffer(surface);
	weston_presentation_feedback_discard_list(&surface->feedback_list);
}

/** weston_compositor_damage_all
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_damage_all(struct weston_compositor *compositor)
{
	struct weston_output *output;

	wl_list_for_each(output, &compositor->output_list, link)
		weston_output_damage(output);
}

/**
 * \ingroup output
 */
WL_EXPORT void
weston_output_damage(struct weston_output *output)
{
	struct weston_compositor *compositor = output->compositor;

	pixman_region32_union(&compositor->primary_plane.damage,
			      &compositor->primary_plane.damage,
			      &output->region);
	weston_output_schedule_repaint(output);
}

static void
surface_flush_damage(struct weston_surface *surface)
{
	if (surface->buffer_ref.buffer &&
	    wl_shm_buffer_get(surface->buffer_ref.buffer->resource))
		surface->compositor->renderer->flush_damage(surface);

	if (pixman_region32_not_empty(&surface->damage))
		TL_POINT(surface->compositor, "core_flush_damage", TLP_SURFACE(surface),
			 TLP_OUTPUT(surface->output), TLP_END);

	pixman_region32_clear(&surface->damage);
}

static void
view_accumulate_damage(struct weston_view *view,
		       pixman_region32_t *opaque)
{
	pixman_region32_t damage;

	pixman_region32_init(&damage);
	if (view->transform.enabled) {
		pixman_box32_t *extents;

		extents = pixman_region32_extents(&view->surface->damage);
		view_compute_bbox(view, extents, &damage);
	} else {
		pixman_region32_copy(&damage, &view->surface->damage);
		pixman_region32_translate(&damage,
					  view->geometry.x, view->geometry.y);
	}

	pixman_region32_intersect(&damage, &damage,
				  &view->transform.boundingbox);
	pixman_region32_subtract(&damage, &damage, opaque);
	pixman_region32_union(&view->plane->damage,
			      &view->plane->damage, &damage);
	pixman_region32_fini(&damage);
	pixman_region32_copy(&view->clip, opaque);
	pixman_region32_union(opaque, opaque, &view->transform.opaque);
}

static void
output_accumulate_damage(struct weston_output *output)
{
	struct weston_compositor *ec = output->compositor;
	struct weston_plane *plane;
	struct weston_view *ev;
	pixman_region32_t opaque, clip;

	pixman_region32_init(&clip);

	wl_list_for_each(plane, &ec->plane_list, link) {
		pixman_region32_copy(&plane->clip, &clip);

		pixman_region32_init(&opaque);

		wl_list_for_each(ev, &ec->view_list, link) {
			if (ev->plane != plane)
				continue;

			view_accumulate_damage(ev, &opaque);
		}

		pixman_region32_union(&clip, &clip, &opaque);
		pixman_region32_fini(&opaque);
	}

	pixman_region32_fini(&clip);

	wl_list_for_each(ev, &ec->view_list, link)
		ev->surface->touched = false;

	wl_list_for_each(ev, &ec->view_list, link) {
		/* Ignore views not visible on the current output */
		if (!(ev->output_mask & (1u << output->id)))
			continue;
		if (ev->surface->touched)
			continue;
		ev->surface->touched = true;

		surface_flush_damage(ev->surface);

		/* Both the renderer and the backend have seen the buffer
		 * by now. If renderer needs the buffer, it has its own
		 * reference set. If the backend wants to keep the buffer
		 * around for migrating the surface into a non-primary plane
		 * later, keep_buffer is true. Otherwise, drop the core
		 * reference now, and allow early buffer release. This enables
		 * clients to use single-buffering.
		 */
		if (!ev->surface->keep_buffer) {
			weston_buffer_reference(&ev->surface->buffer_ref, NULL);
			weston_buffer_release_reference(
				&ev->surface->buffer_release_ref, NULL);
		}
	}
}

static void
surface_stash_subsurface_views(struct weston_surface *surface)
{
	struct weston_subsurface *sub;

	wl_list_for_each(sub, &surface->subsurface_list, parent_link) {
		if (sub->surface == surface)
			continue;

		wl_list_insert_list(&sub->unused_views, &sub->surface->views);
		wl_list_init(&sub->surface->views);

		surface_stash_subsurface_views(sub->surface);
	}
}

static void
surface_free_unused_subsurface_views(struct weston_surface *surface)
{
	struct weston_subsurface *sub;
	struct weston_view *view, *nv;

	wl_list_for_each(sub, &surface->subsurface_list, parent_link) {
		if (sub->surface == surface)
			continue;

		wl_list_for_each_safe(view, nv, &sub->unused_views, surface_link) {
			weston_view_unmap (view);
			weston_view_destroy(view);
		}

		surface_free_unused_subsurface_views(sub->surface);
	}
}

static void
view_list_add_subsurface_view(struct weston_compositor *compositor,
			      struct weston_subsurface *sub,
			      struct weston_view *parent)
{
	struct weston_subsurface *child;
	struct weston_view *view = NULL, *iv;

	if (!weston_surface_is_mapped(sub->surface))
		return;

	wl_list_for_each(iv, &sub->unused_views, surface_link) {
		if (iv->geometry.parent == parent) {
			view = iv;
			break;
		}
	}

	if (view) {
		/* Put it back in the surface's list of views */
		wl_list_remove(&view->surface_link);
		wl_list_insert(&sub->surface->views, &view->surface_link);
	} else {
		view = weston_view_create(sub->surface);
		weston_view_set_position(view,
					 sub->position.x,
					 sub->position.y);
		weston_view_set_transform_parent(view, parent);
	}

	view->parent_view = parent;
	weston_view_update_transform(view);
	view->is_mapped = true;

	if (wl_list_empty(&sub->surface->subsurface_list)) {
		wl_list_insert(compositor->view_list.prev, &view->link);
		return;
	}

	wl_list_for_each(child, &sub->surface->subsurface_list, parent_link) {
		if (child->surface == sub->surface)
			wl_list_insert(compositor->view_list.prev, &view->link);
		else
			view_list_add_subsurface_view(compositor, child, view);
	}
}

/* This recursively adds the sub-surfaces for a view, relying on the
 * sub-surface order. Thus, if a client restacks the sub-surfaces, that
 * change first happens to the sub-surface list, and then automatically
 * propagates here. See weston_surface_damage_subsurfaces() for how the
 * sub-surfaces receive damage when the client changes the state.
 */
static void
view_list_add(struct weston_compositor *compositor,
	      struct weston_view *view)
{
	struct weston_subsurface *sub;

	weston_view_update_transform(view);

	if (wl_list_empty(&view->surface->subsurface_list)) {
		wl_list_insert(compositor->view_list.prev, &view->link);
		return;
	}

	wl_list_for_each(sub, &view->surface->subsurface_list, parent_link) {
		if (sub->surface == view->surface)
			wl_list_insert(compositor->view_list.prev, &view->link);
		else
			view_list_add_subsurface_view(compositor, sub, view);
	}
}

static void
weston_compositor_build_view_list(struct weston_compositor *compositor)
{
	struct weston_view *view, *tmp;
	struct weston_layer *layer;

	wl_list_for_each(layer, &compositor->layer_list, link)
		wl_list_for_each(view, &layer->view_list.link, layer_link.link)
			surface_stash_subsurface_views(view->surface);

	wl_list_for_each_safe(view, tmp, &compositor->view_list, link)
		wl_list_init(&view->link);
	wl_list_init(&compositor->view_list);

	wl_list_for_each(layer, &compositor->layer_list, link) {
		wl_list_for_each(view, &layer->view_list.link, layer_link.link) {
			view_list_add(compositor, view);
		}
	}

	wl_list_for_each(layer, &compositor->layer_list, link)
		wl_list_for_each(view, &layer->view_list.link, layer_link.link)
			surface_free_unused_subsurface_views(view->surface);
}

static void
weston_output_take_feedback_list(struct weston_output *output,
				 struct weston_surface *surface)
{
	struct weston_view *view;
	struct weston_presentation_feedback *feedback;
	uint32_t flags = 0xffffffff;

	if (wl_list_empty(&surface->feedback_list))
		return;

	/* All views must have the flag for the flag to survive. */
	wl_list_for_each(view, &surface->views, surface_link) {
		/* ignore views that are not on this output at all */
		if (view->output_mask & (1u << output->id))
			flags &= view->psf_flags;
	}

	wl_list_for_each(feedback, &surface->feedback_list, link)
		feedback->psf_flags = flags;

	wl_list_insert_list(&output->feedback_list, &surface->feedback_list);
	wl_list_init(&surface->feedback_list);
}

static int
weston_output_repaint(struct weston_output *output, void *repaint_data)
{
	struct weston_compositor *ec = output->compositor;
	struct weston_view *ev;
	struct weston_animation *animation, *next;
	struct weston_frame_callback *cb, *cnext;
	struct wl_list frame_callback_list;
	pixman_region32_t output_damage;
	int r;
	uint32_t frame_time_msec;
	enum weston_hdcp_protection highest_requested = WESTON_HDCP_DISABLE;

	if (output->destroying)
		return 0;

	TL_POINT(ec, "core_repaint_begin", TLP_OUTPUT(output), TLP_END);

	/* Rebuild the surface list and update surface transforms up front. */
	weston_compositor_build_view_list(ec);

	/* Find the highest protection desired for an output */
	wl_list_for_each(ev, &ec->view_list, link) {
		if (ev->surface->output_mask & (1u << output->id)) {
			/*
			 * The desired_protection of the output should be the
			 * maximum of the desired_protection of the surfaces,
			 * that are displayed on that output, to avoid
			 * reducing the protection for existing surfaces.
			 */
			if (ev->surface->desired_protection > highest_requested)
				highest_requested =
						ev->surface->desired_protection;
		}
	}

	output->desired_protection = highest_requested;

	if (output->assign_planes && !output->disable_planes) {
		output->assign_planes(output, repaint_data);
	} else {
		wl_list_for_each(ev, &ec->view_list, link) {
			weston_view_move_to_plane(ev, &ec->primary_plane);
			ev->psf_flags = 0;
		}
	}

	wl_list_init(&frame_callback_list);
	wl_list_for_each(ev, &ec->view_list, link) {
		/* Note: This operation is safe to do multiple times on the
		 * same surface.
		 */
		if (ev->surface->output == output) {
			wl_list_insert_list(&frame_callback_list,
					    &ev->surface->frame_callback_list);
			wl_list_init(&ev->surface->frame_callback_list);

			weston_output_take_feedback_list(output, ev->surface);
		}
	}

	output_accumulate_damage(output);

	pixman_region32_init(&output_damage);
	pixman_region32_intersect(&output_damage,
				  &ec->primary_plane.damage, &output->region);
	pixman_region32_subtract(&output_damage,
				 &output_damage, &ec->primary_plane.clip);

	if (output->dirty)
		weston_output_update_matrix(output);

	r = output->repaint(output, &output_damage, repaint_data);

	pixman_region32_fini(&output_damage);

	output->repaint_needed = false;
	if (r == 0)
		output->repaint_status = REPAINT_AWAITING_COMPLETION;

	weston_compositor_repick(ec);

	frame_time_msec = timespec_to_msec(&output->frame_time);

	wl_list_for_each_safe(cb, cnext, &frame_callback_list, link) {
		wl_callback_send_done(cb->resource, frame_time_msec);
		wl_resource_destroy(cb->resource);
	}

	wl_list_for_each_safe(animation, next, &output->animation_list, link) {
		animation->frame_counter++;
		animation->frame(animation, output, &output->frame_time);
	}

	TL_POINT(ec, "core_repaint_posted", TLP_OUTPUT(output), TLP_END);

	return r;
}

static void
weston_output_schedule_repaint_reset(struct weston_output *output)
{
	output->repaint_status = REPAINT_NOT_SCHEDULED;
	TL_POINT(output->compositor, "core_repaint_exit_loop",
		 TLP_OUTPUT(output), TLP_END);
}

static int
weston_output_maybe_repaint(struct weston_output *output, struct timespec *now,
			    void *repaint_data)
{
	struct weston_compositor *compositor = output->compositor;
	int ret = 0;
	int64_t msec_to_repaint;

	/* We're not ready yet; come back to make a decision later. */
	if (output->repaint_status != REPAINT_SCHEDULED)
		return ret;

	msec_to_repaint = timespec_sub_to_msec(&output->next_repaint, now);
	if (msec_to_repaint > 1)
		return ret;

	/* If we're sleeping, drop the repaint machinery entirely; we will
	 * explicitly repaint all outputs when we come back. */
	if (compositor->state == WESTON_COMPOSITOR_SLEEPING ||
	    compositor->state == WESTON_COMPOSITOR_OFFSCREEN)
		goto err;

	/* We don't actually need to repaint this output; drop it from
	 * repaint until something causes damage. */
	if (!output->repaint_needed)
		goto err;

	/* If repaint fails, we aren't going to get weston_output_finish_frame
	 * to trigger a new repaint, so drop it from repaint and hope
	 * something schedules a successful repaint later. As repainting may
	 * take some time, re-read our clock as a courtesy to the next
	 * output. */
	ret = weston_output_repaint(output, repaint_data);
	weston_compositor_read_presentation_clock(compositor, now);
	if (ret != 0)
		goto err;

	output->repainted = true;
	return ret;

err:
	weston_output_schedule_repaint_reset(output);
	return ret;
}

static void
output_repaint_timer_arm(struct weston_compositor *compositor)
{
	struct weston_output *output;
	bool any_should_repaint = false;
	struct timespec now;
	int64_t msec_to_next = INT64_MAX;

	weston_compositor_read_presentation_clock(compositor, &now);

	wl_list_for_each(output, &compositor->output_list, link) {
		int64_t msec_to_this;

		if (output->repaint_status != REPAINT_SCHEDULED)
			continue;

		msec_to_this = timespec_sub_to_msec(&output->next_repaint,
						    &now);
		if (!any_should_repaint || msec_to_this < msec_to_next)
			msec_to_next = msec_to_this;

		any_should_repaint = true;
	}

	if (!any_should_repaint)
		return;

	/* Even if we should repaint immediately, add the minimum 1 ms delay.
	 * This is a workaround to allow coalescing multiple output repaints
	 * particularly from weston_output_finish_frame()
	 * into the same call, which would not happen if we called
	 * output_repaint_timer_handler() directly.
	 */
	if (msec_to_next < 1)
		msec_to_next = 1;

	wl_event_source_timer_update(compositor->repaint_timer, msec_to_next);
}

static int
output_repaint_timer_handler(void *data)
{
	struct weston_compositor *compositor = data;
	struct weston_output *output;
	struct timespec now;
	void *repaint_data = NULL;
	int ret = 0;

	weston_compositor_read_presentation_clock(compositor, &now);

	if (compositor->backend->repaint_begin)
		repaint_data = compositor->backend->repaint_begin(compositor);

	wl_list_for_each(output, &compositor->output_list, link) {
		ret = weston_output_maybe_repaint(output, &now, repaint_data);
		if (ret)
			break;
	}

	if (ret == 0) {
		if (compositor->backend->repaint_flush)
			ret = compositor->backend->repaint_flush(compositor,
							 repaint_data);
	} else {
		if (compositor->backend->repaint_cancel)
			compositor->backend->repaint_cancel(compositor,
							    repaint_data);
	}

	if (ret != 0) {
		wl_list_for_each(output, &compositor->output_list, link) {
			if (output->repainted)
				weston_output_schedule_repaint_reset(output);
		}
	}

	wl_list_for_each(output, &compositor->output_list, link)
		output->repainted = false;

	output_repaint_timer_arm(compositor);

	return 0;
}

/** Convert a presentation timestamp to another clock domain
 *
 * \param compositor The compositor defines the presentation clock domain.
 * \param presentation_stamp The timestamp in presentation clock domain.
 * \param presentation_now Current time in presentation clock domain.
 * \param target_clock Defines the target clock domain.
 *
 * This approximation relies on presentation_stamp to be close to current time.
 * The further it is from current time and the bigger the speed difference
 * between the two clock domains, the bigger the conversion error.
 *
 * Conversion error due to system load is biased and unbounded.
 */
static struct timespec
convert_presentation_time_now(struct weston_compositor *compositor,
			      const struct timespec *presentation_stamp,
			      const struct timespec *presentation_now,
			      clockid_t target_clock)
{
	struct timespec target_now = {};
	struct timespec target_stamp;
	int64_t delta_ns;

	if (compositor->presentation_clock == target_clock)
		return *presentation_stamp;

	clock_gettime(target_clock, &target_now);
	delta_ns = timespec_sub_to_nsec(presentation_stamp, presentation_now);
	timespec_add_nsec(&target_stamp, &target_now, delta_ns);

	return target_stamp;
}

/**
 * \ingroup output
 */
WL_EXPORT void
weston_output_finish_frame(struct weston_output *output,
			   const struct timespec *stamp,
			   uint32_t presented_flags)
{
	struct weston_compositor *compositor = output->compositor;
	int32_t refresh_nsec;
	struct timespec now;
	struct timespec vblank_monotonic;
	int64_t msec_rel;

	assert(output->repaint_status == REPAINT_AWAITING_COMPLETION);
	assert(stamp || (presented_flags & WP_PRESENTATION_FEEDBACK_INVALID));

	weston_compositor_read_presentation_clock(compositor, &now);

	/* If we haven't been supplied any timestamp at all, we don't have a
	 * timebase to work against, so any delay just wastes time. Push a
	 * repaint as soon as possible so we can get on with it. */
	if (!stamp) {
		output->next_repaint = now;
		goto out;
	}

	vblank_monotonic = convert_presentation_time_now(compositor,
							 stamp, &now,
							 CLOCK_MONOTONIC);
	TL_POINT(compositor, "core_repaint_finished", TLP_OUTPUT(output),
		 TLP_VBLANK(&vblank_monotonic), TLP_END);

	refresh_nsec = millihz_to_nsec(output->current_mode->refresh);
	weston_presentation_feedback_present_list(&output->feedback_list,
						  output, refresh_nsec, stamp,
						  output->msc,
						  presented_flags);

	output->frame_time = *stamp;

	timespec_add_nsec(&output->next_repaint, stamp, refresh_nsec);
	timespec_add_msec(&output->next_repaint, &output->next_repaint,
			  -compositor->repaint_msec);
	msec_rel = timespec_sub_to_msec(&output->next_repaint, &now);

	if (msec_rel < -1000 || msec_rel > 1000) {
		static bool warned;

		if (!warned)
			weston_log("Warning: computed repaint delay is "
				   "insane: %lld msec\n", (long long) msec_rel);
		warned = true;

		output->next_repaint = now;
	}

	/* Called from restart_repaint_loop and restart happens already after
	 * the deadline given by repaint_msec? In that case we delay until
	 * the deadline of the next frame, to give clients a more predictable
	 * timing of the repaint cycle to lock on. */
	if (presented_flags == WP_PRESENTATION_FEEDBACK_INVALID &&
	    msec_rel < 0) {
		while (timespec_sub_to_nsec(&output->next_repaint, &now) < 0) {
			timespec_add_nsec(&output->next_repaint,
					  &output->next_repaint,
					  refresh_nsec);
		}
	}

out:
	output->repaint_status = REPAINT_SCHEDULED;
	output_repaint_timer_arm(compositor);
}

static void
idle_repaint(void *data)
{
	struct weston_output *output = data;
	int ret;

	assert(output->repaint_status == REPAINT_BEGIN_FROM_IDLE);
	output->repaint_status = REPAINT_AWAITING_COMPLETION;
	output->idle_repaint_source = NULL;
	ret = output->start_repaint_loop(output);
	if (ret != 0)
		weston_output_schedule_repaint_reset(output);
}

WL_EXPORT void
weston_layer_entry_insert(struct weston_layer_entry *list,
			  struct weston_layer_entry *entry)
{
	wl_list_insert(&list->link, &entry->link);
	entry->layer = list->layer;
}

WL_EXPORT void
weston_layer_entry_remove(struct weston_layer_entry *entry)
{
	wl_list_remove(&entry->link);
	wl_list_init(&entry->link);
	entry->layer = NULL;
}


/** Initialize the weston_layer struct.
 *
 * \param compositor The compositor instance
 * \param layer The layer to initialize
 */
WL_EXPORT void
weston_layer_init(struct weston_layer *layer,
		  struct weston_compositor *compositor)
{
	layer->compositor = compositor;
	wl_list_init(&layer->link);
	wl_list_init(&layer->view_list.link);
	layer->view_list.layer = layer;
	weston_layer_set_mask_infinite(layer);
}

/** Sets the position of the layer in the layer list. The layer will be placed
 * below any layer with the same position value, if any.
 * This function is safe to call if the layer is already on the list, but the
 * layer may be moved below other layers at the same position, if any.
 *
 * \param layer The layer to modify
 * \param position The position the layer will be placed at
 */
WL_EXPORT void
weston_layer_set_position(struct weston_layer *layer,
			  enum weston_layer_position position)
{
	struct weston_layer *below;

	wl_list_remove(&layer->link);

	/* layer_list is ordered from top to bottom, the last layer being the
	 * background with the smallest position value */

	layer->position = position;
	wl_list_for_each_reverse(below, &layer->compositor->layer_list, link) {
		if (below->position >= layer->position) {
			wl_list_insert(&below->link, &layer->link);
			return;
		}
	}
	wl_list_insert(&layer->compositor->layer_list, &layer->link);
}

/** Hide a layer by taking it off the layer list.
 * This function is safe to call if the layer is not on the list.
 *
 * \param layer The layer to hide
 */
WL_EXPORT void
weston_layer_unset_position(struct weston_layer *layer)
{
	wl_list_remove(&layer->link);
	wl_list_init(&layer->link);
}

WL_EXPORT void
weston_layer_set_mask(struct weston_layer *layer,
		      int x, int y, int width, int height)
{
	struct weston_view *view;

	layer->mask.x1 = x;
	layer->mask.x2 = x + width;
	layer->mask.y1 = y;
	layer->mask.y2 = y + height;

	wl_list_for_each(view, &layer->view_list.link, layer_link.link) {
		weston_view_geometry_dirty(view);
	}
}

WL_EXPORT void
weston_layer_set_mask_infinite(struct weston_layer *layer)
{
	struct weston_view *view;

	layer->mask.x1 = INT32_MIN;
	layer->mask.x2 = INT32_MAX;
	layer->mask.y1 = INT32_MIN;
	layer->mask.y2 = INT32_MAX;

	wl_list_for_each(view, &layer->view_list.link, layer_link.link) {
		weston_view_geometry_dirty(view);
	}
}

WL_EXPORT bool
weston_layer_mask_is_infinite(struct weston_layer *layer)
{
	return layer->mask.x1 == INT32_MIN &&
	       layer->mask.y1 == INT32_MIN &&
	       layer->mask.x2 == INT32_MAX &&
	       layer->mask.y2 == INT32_MAX;
}

/**
 * \ingroup output
 */
WL_EXPORT void
weston_output_schedule_repaint(struct weston_output *output)
{
	struct weston_compositor *compositor = output->compositor;
	struct wl_event_loop *loop;

	if (compositor->state == WESTON_COMPOSITOR_SLEEPING ||
	    compositor->state == WESTON_COMPOSITOR_OFFSCREEN)
		return;

	if (!output->repaint_needed)
		TL_POINT(compositor, "core_repaint_req", TLP_OUTPUT(output), TLP_END);

	loop = wl_display_get_event_loop(compositor->wl_display);
	output->repaint_needed = true;

	/* If we already have a repaint scheduled for our idle handler,
	 * no need to set it again. If the repaint has been called but
	 * not finished, then weston_output_finish_frame() will notice
	 * that a repaint is needed and schedule one. */
	if (output->repaint_status != REPAINT_NOT_SCHEDULED)
		return;

	output->repaint_status = REPAINT_BEGIN_FROM_IDLE;
	assert(!output->idle_repaint_source);
	output->idle_repaint_source = wl_event_loop_add_idle(loop, idle_repaint,
							     output);
	TL_POINT(compositor, "core_repaint_enter_loop", TLP_OUTPUT(output), TLP_END);
}

/** weston_compositor_schedule_repaint
 *  \ingroup compositor
 */
WL_EXPORT void
weston_compositor_schedule_repaint(struct weston_compositor *compositor)
{
	struct weston_output *output;

	wl_list_for_each(output, &compositor->output_list, link)
		weston_output_schedule_repaint(output);
}

static void
surface_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
surface_attach(struct wl_client *client,
	       struct wl_resource *resource,
	       struct wl_resource *buffer_resource, int32_t sx, int32_t sy)
{
	struct weston_surface *surface = wl_resource_get_user_data(resource);
	struct weston_buffer *buffer = NULL;

	if (buffer_resource) {
		buffer = weston_buffer_from_resource(buffer_resource);
		if (buffer == NULL) {
			wl_client_post_no_memory(client);
			return;
		}
	}

	/* Attach, attach, without commit in between does not send
	 * wl_buffer.release. */
	weston_surface_state_set_buffer(&surface->pending, buffer);

	surface->pending.sx = sx;
	surface->pending.sy = sy;
	surface->pending.newly_attached = 1;
}

static void
surface_damage(struct wl_client *client,
	       struct wl_resource *resource,
	       int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct weston_surface *surface = wl_resource_get_user_data(resource);

	if (width <= 0 || height <= 0)
		return;

	pixman_region32_union_rect(&surface->pending.damage_surface,
				   &surface->pending.damage_surface,
				   x, y, width, height);
}

static void
surface_damage_buffer(struct wl_client *client,
		      struct wl_resource *resource,
		      int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct weston_surface *surface = wl_resource_get_user_data(resource);

	if (width <= 0 || height <= 0)
		return;

	pixman_region32_union_rect(&surface->pending.damage_buffer,
				   &surface->pending.damage_buffer,
				   x, y, width, height);
}

static void
destroy_frame_callback(struct wl_resource *resource)
{
	struct weston_frame_callback *cb = wl_resource_get_user_data(resource);

	wl_list_remove(&cb->link);
	free(cb);
}

static void
surface_frame(struct wl_client *client,
	      struct wl_resource *resource, uint32_t callback)
{
	struct weston_frame_callback *cb;
	struct weston_surface *surface = wl_resource_get_user_data(resource);

	cb = malloc(sizeof *cb);
	if (cb == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	cb->resource = wl_resource_create(client, &wl_callback_interface, 1,
					  callback);
	if (cb->resource == NULL) {
		free(cb);
		wl_resource_post_no_memory(resource);
		return;
	}

	wl_resource_set_implementation(cb->resource, NULL, cb,
				       destroy_frame_callback);

	wl_list_insert(surface->pending.frame_callback_list.prev, &cb->link);
}

static void
surface_set_opaque_region(struct wl_client *client,
			  struct wl_resource *resource,
			  struct wl_resource *region_resource)
{
	struct weston_surface *surface = wl_resource_get_user_data(resource);
	struct weston_region *region;

	if (region_resource) {
		region = wl_resource_get_user_data(region_resource);
		pixman_region32_copy(&surface->pending.opaque,
				     &region->region);
	} else {
		pixman_region32_clear(&surface->pending.opaque);
	}
}

static void
surface_set_input_region(struct wl_client *client,
			 struct wl_resource *resource,
			 struct wl_resource *region_resource)
{
	struct weston_surface *surface = wl_resource_get_user_data(resource);
	struct weston_region *region;

	if (region_resource) {
		region = wl_resource_get_user_data(region_resource);
		pixman_region32_copy(&surface->pending.input,
				     &region->region);
	} else {
		pixman_region32_fini(&surface->pending.input);
		region_init_infinite(&surface->pending.input);
	}
}

/* Cause damage to this sub-surface and all its children.
 *
 * This is useful when there are state changes that need an implicit
 * damage, e.g. a z-order change.
 */
static void
weston_surface_damage_subsurfaces(struct weston_subsurface *sub)
{
	struct weston_subsurface *child;

	weston_surface_damage(sub->surface);
	sub->reordered = false;

	wl_list_for_each(child, &sub->surface->subsurface_list, parent_link)
		if (child != sub)
			weston_surface_damage_subsurfaces(child);
}

static void
weston_surface_commit_subsurface_order(struct weston_surface *surface)
{
	struct weston_subsurface *sub;

	wl_list_for_each_reverse(sub, &surface->subsurface_list_pending,
				 parent_link_pending) {
		wl_list_remove(&sub->parent_link);
		wl_list_insert(&surface->subsurface_list, &sub->parent_link);

		if (sub->reordered)
			weston_surface_damage_subsurfaces(sub);
	}
}

static void
weston_surface_build_buffer_matrix(const struct weston_surface *surface,
				   struct weston_matrix *matrix)
{
	const struct weston_buffer_viewport *vp = &surface->buffer_viewport;
	double src_width, src_height, dest_width, dest_height;

	weston_matrix_init(matrix);

	if (vp->buffer.src_width == wl_fixed_from_int(-1)) {
		src_width = surface->width_from_buffer;
		src_height = surface->height_from_buffer;
	} else {
		src_width = wl_fixed_to_double(vp->buffer.src_width);
		src_height = wl_fixed_to_double(vp->buffer.src_height);
	}

	if (vp->surface.width == -1) {
		dest_width = src_width;
		dest_height = src_height;
	} else {
		dest_width = vp->surface.width;
		dest_height = vp->surface.height;
	}

	if (src_width != dest_width || src_height != dest_height)
		weston_matrix_scale(matrix,
				    src_width / dest_width,
				    src_height / dest_height, 1);

	if (vp->buffer.src_width != wl_fixed_from_int(-1))
		weston_matrix_translate(matrix,
					wl_fixed_to_double(vp->buffer.src_x),
					wl_fixed_to_double(vp->buffer.src_y),
					0);

	switch (vp->buffer.transform) {
	case WL_OUTPUT_TRANSFORM_FLIPPED:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		weston_matrix_scale(matrix, -1, 1, 1);
		weston_matrix_translate(matrix,
					surface->width_from_buffer, 0, 0);
		break;
	}

	switch (vp->buffer.transform) {
	default:
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		break;
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		weston_matrix_rotate_xy(matrix, 0, -1);
		weston_matrix_translate(matrix,
					0, surface->width_from_buffer, 0);
		break;
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		weston_matrix_rotate_xy(matrix, -1, 0);
		weston_matrix_translate(matrix,
					surface->width_from_buffer,
					surface->height_from_buffer, 0);
		break;
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		weston_matrix_rotate_xy(matrix, 0, 1);
		weston_matrix_translate(matrix,
					surface->height_from_buffer, 0, 0);
		break;
	}

	weston_matrix_scale(matrix, vp->buffer.scale, vp->buffer.scale, 1);
}

/**
 * Compute a + b > c while being safe to overflows.
 */
static bool
fixed_sum_gt(wl_fixed_t a, wl_fixed_t b, wl_fixed_t c)
{
	return (int64_t)a + (int64_t)b > (int64_t)c;
}

static bool
weston_surface_is_pending_viewport_source_valid(
	const struct weston_surface *surface)
{
	const struct weston_surface_state *pend = &surface->pending;
	const struct weston_buffer_viewport *vp = &pend->buffer_viewport;
	int width_from_buffer = 0;
	int height_from_buffer = 0;
	wl_fixed_t w;
	wl_fixed_t h;

	/* If viewport source rect is not set, it is always ok. */
	if (vp->buffer.src_width == wl_fixed_from_int(-1))
		return true;

	if (pend->newly_attached) {
		if (pend->buffer) {
			convert_size_by_transform_scale(&width_from_buffer,
							&height_from_buffer,
							pend->buffer->width,
							pend->buffer->height,
							vp->buffer.transform,
							vp->buffer.scale);
		} else {
			/* No buffer: viewport is irrelevant. */
			return true;
		}
	} else {
		width_from_buffer = surface->width_from_buffer;
		height_from_buffer = surface->height_from_buffer;
	}

	assert((width_from_buffer == 0) == (height_from_buffer == 0));
	assert(width_from_buffer >= 0 && height_from_buffer >= 0);

	/* No buffer: viewport is irrelevant. */
	if (width_from_buffer == 0 || height_from_buffer == 0)
		return true;

	/* overflow checks for wl_fixed_from_int() */
	if (width_from_buffer > wl_fixed_to_int(INT32_MAX))
		return false;
	if (height_from_buffer > wl_fixed_to_int(INT32_MAX))
		return false;

	w = wl_fixed_from_int(width_from_buffer);
	h = wl_fixed_from_int(height_from_buffer);

	if (fixed_sum_gt(vp->buffer.src_x, vp->buffer.src_width, w))
		return false;
	if (fixed_sum_gt(vp->buffer.src_y, vp->buffer.src_height, h))
		return false;

	return true;
}

static bool
fixed_is_integer(wl_fixed_t v)
{
	return (v & 0xff) == 0;
}

static bool
weston_surface_is_pending_viewport_dst_size_int(
	const struct weston_surface *surface)
{
	const struct weston_buffer_viewport *vp =
		&surface->pending.buffer_viewport;

	if (vp->surface.width != -1) {
		assert(vp->surface.width > 0 && vp->surface.height > 0);
		return true;
	}

	return fixed_is_integer(vp->buffer.src_width) &&
	       fixed_is_integer(vp->buffer.src_height);
}

/* Translate pending damage in buffer co-ordinates to surface
 * co-ordinates and union it with a pixman_region32_t.
 * This should only be called after the buffer is attached.
 */
static void
apply_damage_buffer(pixman_region32_t *dest,
		    struct weston_surface *surface,
		    struct weston_surface_state *state)
{
	struct weston_buffer *buffer = surface->buffer_ref.buffer;

	/* wl_surface.damage_buffer needs to be clipped to the buffer,
	 * translated into surface co-ordinates and unioned with
	 * any other surface damage.
	 * None of this makes sense if there is no buffer though.
	 */
	if (buffer && pixman_region32_not_empty(&state->damage_buffer)) {
		pixman_region32_t buffer_damage;

		pixman_region32_intersect_rect(&state->damage_buffer,
					       &state->damage_buffer,
					       0, 0, buffer->width,
					       buffer->height);
		pixman_region32_init(&buffer_damage);
		weston_matrix_transform_region(&buffer_damage,
					       &surface->buffer_to_surface_matrix,
					       &state->damage_buffer);
		pixman_region32_union(dest, dest, &buffer_damage);
		pixman_region32_fini(&buffer_damage);
	}
	/* We should clear this on commit even if there was no buffer */
	pixman_region32_clear(&state->damage_buffer);
}

static void
weston_surface_set_desired_protection(struct weston_surface *surface,
				      enum weston_hdcp_protection protection)
{
	if (surface->desired_protection == protection)
		return;
	surface->desired_protection = protection;
	weston_surface_damage(surface);
}

static void
weston_surface_set_protection_mode(struct weston_surface *surface,
				   enum weston_surface_protection_mode p_mode)
{
	struct content_protection *cp = surface->compositor->content_protection;
	struct protected_surface *psurface;

	surface->protection_mode = p_mode;
	wl_list_for_each(psurface, &cp->protected_list, link) {
		if (!psurface || psurface->surface != surface)
			continue;
		weston_protected_surface_send_event(psurface,
						    surface->current_protection);
	}
}

static void
weston_surface_commit_state(struct weston_surface *surface,
			    struct weston_surface_state *state)
{
	struct weston_view *view;
	pixman_region32_t opaque;

	/* wl_surface.set_buffer_transform */
	/* wl_surface.set_buffer_scale */
	/* wp_viewport.set_source */
	/* wp_viewport.set_destination */
	surface->buffer_viewport = state->buffer_viewport;

	/* wl_surface.attach */
	if (state->newly_attached) {
		/* zwp_surface_synchronization_v1.set_acquire_fence */
		fd_move(&surface->acquire_fence_fd,
			&state->acquire_fence_fd);
		/* zwp_surface_synchronization_v1.get_release */
		weston_buffer_release_move(&surface->buffer_release_ref,
					   &state->buffer_release_ref);
		weston_surface_attach(surface, state->buffer);
	}
	weston_surface_state_set_buffer(state, NULL);
	assert(state->acquire_fence_fd == -1);
	assert(state->buffer_release_ref.buffer_release == NULL);

	weston_surface_build_buffer_matrix(surface,
					   &surface->surface_to_buffer_matrix);
	weston_matrix_invert(&surface->buffer_to_surface_matrix,
			     &surface->surface_to_buffer_matrix);

	if (state->newly_attached || state->buffer_viewport.changed) {
		weston_surface_update_size(surface);
		if (surface->committed)
			surface->committed(surface, state->sx, state->sy);
	}

	state->sx = 0;
	state->sy = 0;
	state->newly_attached = 0;
	state->buffer_viewport.changed = 0;

	/* wl_surface.damage and wl_surface.damage_buffer */
	if (pixman_region32_not_empty(&state->damage_surface) ||
	     pixman_region32_not_empty(&state->damage_buffer))
		TL_POINT(surface->compositor, "core_commit_damage", TLP_SURFACE(surface), TLP_END);

	pixman_region32_union(&surface->damage, &surface->damage,
			      &state->damage_surface);

	apply_damage_buffer(&surface->damage, surface, state);

	pixman_region32_intersect_rect(&surface->damage, &surface->damage,
				       0, 0, surface->width, surface->height);
	pixman_region32_clear(&state->damage_surface);

	/* wl_surface.set_opaque_region */
	pixman_region32_init(&opaque);
	pixman_region32_intersect_rect(&opaque, &state->opaque,
				       0, 0, surface->width, surface->height);

	if (!pixman_region32_equal(&opaque, &surface->opaque)) {
		pixman_region32_copy(&surface->opaque, &opaque);
		wl_list_for_each(view, &surface->views, surface_link)
			weston_view_geometry_dirty(view);
	}

	pixman_region32_fini(&opaque);

	/* wl_surface.set_input_region */
	pixman_region32_intersect_rect(&surface->input, &state->input,
				       0, 0, surface->width, surface->height);

	/* wl_surface.frame */
	wl_list_insert_list(&surface->frame_callback_list,
			    &state->frame_callback_list);
	wl_list_init(&state->frame_callback_list);

	/* XXX:
	 * What should happen with a feedback request, if there
	 * is no wl_buffer attached for this commit?
	 */

	/* presentation.feedback */
	wl_list_insert_list(&surface->feedback_list,
			    &state->feedback_list);
	wl_list_init(&state->feedback_list);

	/* weston_protected_surface.enforced/relaxed */
	if (surface->protection_mode != state->protection_mode)
		weston_surface_set_protection_mode(surface,
						   state->protection_mode);

	/* weston_protected_surface.set_type */
	weston_surface_set_desired_protection(surface, state->desired_protection);

	wl_signal_emit(&surface->commit_signal, surface);
}

static void
weston_surface_commit(struct weston_surface *surface)
{
	weston_surface_commit_state(surface, &surface->pending);

	weston_surface_commit_subsurface_order(surface);

	weston_surface_schedule_repaint(surface);
}

static void
weston_subsurface_commit(struct weston_subsurface *sub);

static void
weston_subsurface_parent_commit(struct weston_subsurface *sub,
				int parent_is_synchronized);

static void
surface_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct weston_surface *surface = wl_resource_get_user_data(resource);
	struct weston_subsurface *sub = weston_surface_to_subsurface(surface);

	if (!weston_surface_is_pending_viewport_source_valid(surface)) {
		assert(surface->viewport_resource);

		wl_resource_post_error(surface->viewport_resource,
			WP_VIEWPORT_ERROR_OUT_OF_BUFFER,
			"wl_surface@%d has viewport source outside buffer",
			wl_resource_get_id(resource));
		return;
	}

	if (!weston_surface_is_pending_viewport_dst_size_int(surface)) {
		assert(surface->viewport_resource);

		wl_resource_post_error(surface->viewport_resource,
			WP_VIEWPORT_ERROR_BAD_SIZE,
			"wl_surface@%d viewport dst size not integer",
			wl_resource_get_id(resource));
		return;
	}

	if (surface->pending.acquire_fence_fd >= 0) {
		assert(surface->synchronization_resource);

		if (!surface->pending.buffer) {
			fd_clear(&surface->pending.acquire_fence_fd);
			wl_resource_post_error(surface->synchronization_resource,
				ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_NO_BUFFER,
				"wl_surface@%"PRIu32" no buffer for synchronization",
				wl_resource_get_id(resource));
			return;
		}

		/* We support fences for both wp_linux_dmabuf and opaque EGL
		 * buffers, as mandated by minor version 2 of the
		 * zwp_linux_explicit_synchronization_v1 protocol. Since
		 * renderers that support fences currently only support these
		 * two buffer types plus SHM buffers, we can just check for the
		 * SHM buffer case here.
		 */
		if (wl_shm_buffer_get(surface->pending.buffer->resource)) {
			fd_clear(&surface->pending.acquire_fence_fd);
			wl_resource_post_error(surface->synchronization_resource,
				ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_UNSUPPORTED_BUFFER,
				"wl_surface@%"PRIu32" unsupported buffer for synchronization",
				wl_resource_get_id(resource));
			return;
		}
	}

	if (surface->pending.buffer_release_ref.buffer_release &&
	    !surface->pending.buffer) {
		assert(surface->synchronization_resource);

		wl_resource_post_error(surface->synchronization_resource,
			ZWP_LINUX_SURFACE_SYNCHRONIZATION_V1_ERROR_NO_BUFFER,
			"wl_surface@%"PRIu32" no buffer for synchronization",
			wl_resource_get_id(resource));
		return;
	}

	if (sub) {
		weston_subsurface_commit(sub);
		return;
	}

	weston_surface_commit(surface);

	wl_list_for_each(sub, &surface->subsurface_list, parent_link) {
		if (sub->surface != surface)
			weston_subsurface_parent_commit(sub, 0);
	}
}

static void
surface_set_buffer_transform(struct wl_client *client,
			     struct wl_resource *resource, int transform)
{
	struct weston_surface *surface = wl_resource_get_user_data(resource);

	/* if wl_output.transform grows more members this will need to be updated. */
	if (transform < 0 ||
	    transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		wl_resource_post_error(resource,
			WL_SURFACE_ERROR_INVALID_TRANSFORM,
			"buffer transform must be a valid transform "
			"('%d' specified)", transform);
		return;
	}

	surface->pending.buffer_viewport.buffer.transform = transform;
	surface->pending.buffer_viewport.changed = 1;
}

static void
surface_set_buffer_scale(struct wl_client *client,
			 struct wl_resource *resource,
			 int32_t scale)
{
	struct weston_surface *surface = wl_resource_get_user_data(resource);

	if (scale < 1) {
		wl_resource_post_error(resource,
			WL_SURFACE_ERROR_INVALID_SCALE,
			"buffer scale must be at least one "
			"('%d' specified)", scale);
		return;
	}

	surface->pending.buffer_viewport.buffer.scale = scale;
	surface->pending.buffer_viewport.changed = 1;
}

static const struct wl_surface_interface surface_interface = {
	surface_destroy,
	surface_attach,
	surface_damage,
	surface_frame,
	surface_set_opaque_region,
	surface_set_input_region,
	surface_commit,
	surface_set_buffer_transform,
	surface_set_buffer_scale,
	surface_damage_buffer
};

static void
compositor_create_surface(struct wl_client *client,
			  struct wl_resource *resource, uint32_t id)
{
	struct weston_compositor *ec = wl_resource_get_user_data(resource);
	struct weston_surface *surface;

	surface = weston_surface_create(ec);
	if (surface == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	surface->resource =
		wl_resource_create(client, &wl_surface_interface,
				   wl_resource_get_version(resource), id);
	if (surface->resource == NULL) {
		weston_surface_destroy(surface);
		wl_resource_post_no_memory(resource);
		return;
	}
	wl_resource_set_implementation(surface->resource, &surface_interface,
				       surface, destroy_surface);

	wl_signal_emit(&ec->create_surface_signal, surface);
}

static void
destroy_region(struct wl_resource *resource)
{
	struct weston_region *region = wl_resource_get_user_data(resource);

	pixman_region32_fini(&region->region);
	free(region);
}

static void
region_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
region_add(struct wl_client *client, struct wl_resource *resource,
	   int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct weston_region *region = wl_resource_get_user_data(resource);

	pixman_region32_union_rect(&region->region, &region->region,
				   x, y, width, height);
}

static void
region_subtract(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct weston_region *region = wl_resource_get_user_data(resource);
	pixman_region32_t rect;

	pixman_region32_init_rect(&rect, x, y, width, height);
	pixman_region32_subtract(&region->region, &region->region, &rect);
	pixman_region32_fini(&rect);
}

static const struct wl_region_interface region_interface = {
	region_destroy,
	region_add,
	region_subtract
};

static void
compositor_create_region(struct wl_client *client,
			 struct wl_resource *resource, uint32_t id)
{
	struct weston_region *region;

	region = malloc(sizeof *region);
	if (region == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	pixman_region32_init(&region->region);

	region->resource =
		wl_resource_create(client, &wl_region_interface, 1, id);
	if (region->resource == NULL) {
		free(region);
		wl_resource_post_no_memory(resource);
		return;
	}
	wl_resource_set_implementation(region->resource, &region_interface,
				       region, destroy_region);
}

static const struct wl_compositor_interface compositor_interface = {
	compositor_create_surface,
	compositor_create_region
};

static void
weston_subsurface_commit_from_cache(struct weston_subsurface *sub)
{
	struct weston_surface *surface = sub->surface;

	weston_surface_commit_state(surface, &sub->cached);
	weston_buffer_reference(&sub->cached_buffer_ref, NULL);

	weston_surface_commit_subsurface_order(surface);

	weston_surface_schedule_repaint(surface);

	sub->has_cached_data = 0;
}

static void
weston_subsurface_commit_to_cache(struct weston_subsurface *sub)
{
	struct weston_surface *surface = sub->surface;

	/*
	 * If this commit would cause the surface to move by the
	 * attach(dx, dy) parameters, the old damage region must be
	 * translated to correspond to the new surface coordinate system
	 * origin.
	 */
	pixman_region32_translate(&sub->cached.damage_surface,
				  -surface->pending.sx, -surface->pending.sy);
	pixman_region32_union(&sub->cached.damage_surface,
			      &sub->cached.damage_surface,
			      &surface->pending.damage_surface);
	pixman_region32_clear(&surface->pending.damage_surface);

	if (surface->pending.newly_attached) {
		sub->cached.newly_attached = 1;
		weston_surface_state_set_buffer(&sub->cached,
						surface->pending.buffer);
		weston_buffer_reference(&sub->cached_buffer_ref,
					surface->pending.buffer);
		weston_presentation_feedback_discard_list(
					&sub->cached.feedback_list);
		/* zwp_surface_synchronization_v1.set_acquire_fence */
		fd_move(&sub->cached.acquire_fence_fd,
			&surface->pending.acquire_fence_fd);
		/* zwp_surface_synchronization_v1.get_release */
		weston_buffer_release_move(&sub->cached.buffer_release_ref,
					   &surface->pending.buffer_release_ref);
	}
	sub->cached.desired_protection = surface->pending.desired_protection;
	sub->cached.protection_mode = surface->pending.protection_mode;
	assert(surface->pending.acquire_fence_fd == -1);
	assert(surface->pending.buffer_release_ref.buffer_release == NULL);
	sub->cached.sx += surface->pending.sx;
	sub->cached.sy += surface->pending.sy;

	apply_damage_buffer(&sub->cached.damage_surface, surface, &surface->pending);

	sub->cached.buffer_viewport.changed |=
		surface->pending.buffer_viewport.changed;
	sub->cached.buffer_viewport.buffer =
		surface->pending.buffer_viewport.buffer;
	sub->cached.buffer_viewport.surface =
		surface->pending.buffer_viewport.surface;

	weston_surface_reset_pending_buffer(surface);

	pixman_region32_copy(&sub->cached.opaque, &surface->pending.opaque);

	pixman_region32_copy(&sub->cached.input, &surface->pending.input);

	wl_list_insert_list(&sub->cached.frame_callback_list,
			    &surface->pending.frame_callback_list);
	wl_list_init(&surface->pending.frame_callback_list);

	wl_list_insert_list(&sub->cached.feedback_list,
			    &surface->pending.feedback_list);
	wl_list_init(&surface->pending.feedback_list);

	sub->has_cached_data = 1;
}

static bool
weston_subsurface_is_synchronized(struct weston_subsurface *sub)
{
	while (sub) {
		if (sub->synchronized)
			return true;

		if (!sub->parent)
			return false;

		sub = weston_surface_to_subsurface(sub->parent);
	}

	return false;
}

static void
weston_subsurface_commit(struct weston_subsurface *sub)
{
	struct weston_surface *surface = sub->surface;
	struct weston_subsurface *tmp;

	/* Recursive check for effectively synchronized. */
	if (weston_subsurface_is_synchronized(sub)) {
		weston_subsurface_commit_to_cache(sub);
	} else {
		if (sub->has_cached_data) {
			/* flush accumulated state from cache */
			weston_subsurface_commit_to_cache(sub);
			weston_subsurface_commit_from_cache(sub);
		} else {
			weston_surface_commit(surface);
		}

		wl_list_for_each(tmp, &surface->subsurface_list, parent_link) {
			if (tmp->surface != surface)
				weston_subsurface_parent_commit(tmp, 0);
		}
	}
}

static void
weston_subsurface_synchronized_commit(struct weston_subsurface *sub)
{
	struct weston_surface *surface = sub->surface;
	struct weston_subsurface *tmp;

	/* From now on, commit_from_cache the whole sub-tree, regardless of
	 * the synchronized mode of each child. This sub-surface or some
	 * of its ancestors were synchronized, so we are synchronized
	 * all the way down.
	 */

	if (sub->has_cached_data)
		weston_subsurface_commit_from_cache(sub);

	wl_list_for_each(tmp, &surface->subsurface_list, parent_link) {
		if (tmp->surface != surface)
			weston_subsurface_parent_commit(tmp, 1);
	}
}

static void
weston_subsurface_parent_commit(struct weston_subsurface *sub,
				int parent_is_synchronized)
{
	struct weston_view *view;
	if (sub->position.set) {
		wl_list_for_each(view, &sub->surface->views, surface_link)
			weston_view_set_position(view,
						 sub->position.x,
						 sub->position.y);

		sub->position.set = 0;
	}

	if (parent_is_synchronized || sub->synchronized)
		weston_subsurface_synchronized_commit(sub);
}

static int
subsurface_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	return snprintf(buf, len, "sub-surface");
}

static void
subsurface_committed(struct weston_surface *surface, int32_t dx, int32_t dy)
{
	struct weston_view *view;

	wl_list_for_each(view, &surface->views, surface_link)
		weston_view_set_position(view,
					 view->geometry.x + dx,
					 view->geometry.y + dy);

	/* No need to check parent mappedness, because if parent is not
	 * mapped, parent is not in a visible layer, so this sub-surface
	 * will not be drawn either.
	 */

	if (!weston_surface_is_mapped(surface)) {
		surface->is_mapped = true;

		/* Cannot call weston_view_update_transform(),
		 * because that would call it also for the parent surface,
		 * which might not be mapped yet. That would lead to
		 * inconsistent state, where the window could never be
		 * mapped.
		 *
		 * Instead just force the is_mapped flag on, to make
		 * weston_surface_is_mapped() return true, so that when the
		 * parent surface does get mapped, this one will get
		 * included, too. See view_list_add().
		 */
	}
}

static struct weston_subsurface *
weston_surface_to_subsurface(struct weston_surface *surface)
{
	if (surface->committed == subsurface_committed)
		return surface->committed_private;

	return NULL;
}

WL_EXPORT struct weston_surface *
weston_surface_get_main_surface(struct weston_surface *surface)
{
	struct weston_subsurface *sub;

	while (surface && (sub = weston_surface_to_subsurface(surface)))
		surface = sub->parent;

	return surface;
}

WL_EXPORT int
weston_surface_set_role(struct weston_surface *surface,
			const char *role_name,
			struct wl_resource *error_resource,
			uint32_t error_code)
{
	assert(role_name);

	if (surface->role_name == NULL ||
	    surface->role_name == role_name ||
	    strcmp(surface->role_name, role_name) == 0) {
		surface->role_name = role_name;

		return 0;
	}

	wl_resource_post_error(error_resource, error_code,
			       "Cannot assign role %s to wl_surface@%d,"
			       " already has role %s\n",
			       role_name,
			       wl_resource_get_id(surface->resource),
			       surface->role_name);
	return -1;
}

WL_EXPORT const char *
weston_surface_get_role(struct weston_surface *surface)
{
	return surface->role_name;
}

WL_EXPORT void
weston_surface_set_label_func(struct weston_surface *surface,
			      int (*desc)(struct weston_surface *,
					  char *, size_t))
{
	surface->get_label = desc;
	weston_timeline_refresh_subscription_objects(surface->compositor,
						     surface);
}

/** Get the size of surface contents
 *
 * \param surface The surface to query.
 * \param width Returns the width of raw contents.
 * \param height Returns the height of raw contents.
 *
 * Retrieves the raw surface content size in pixels for the given surface.
 * This is the whole content size in buffer pixels. If the surface
 * has no content or the renderer does not implement this feature,
 * zeroes are returned.
 *
 * This function is used to determine the buffer size needed for
 * a weston_surface_copy_content() call.
 */
WL_EXPORT void
weston_surface_get_content_size(struct weston_surface *surface,
				int *width, int *height)
{
	struct weston_renderer *rer = surface->compositor->renderer;

	if (!rer->surface_get_content_size) {
		*width = 0;
		*height = 0;
		return;
	}

	rer->surface_get_content_size(surface, width, height);
}

/** Get the bounding box of a surface and its subsurfaces
 *
 * \param surface The surface to query.
 * \return The bounding box relative to the surface origin.
 *
 */
WL_EXPORT struct weston_geometry
weston_surface_get_bounding_box(struct weston_surface *surface)
{
	pixman_region32_t region;
	pixman_box32_t *box;
	struct weston_subsurface *subsurface;

	pixman_region32_init_rect(&region,
				  0, 0,
				  surface->width, surface->height);

	wl_list_for_each(subsurface, &surface->subsurface_list, parent_link)
		pixman_region32_union_rect(&region, &region,
					   subsurface->position.x,
					   subsurface->position.y,
					   subsurface->surface->width,
					   subsurface->surface->height);

	box = pixman_region32_extents(&region);
	struct weston_geometry geometry = {
		.x = box->x1,
		.y = box->y1,
		.width = box->x2 - box->x1,
		.height = box->y2 - box->y1,
	};

	pixman_region32_fini(&region);

	return geometry;
}

/** Copy surface contents to system memory.
 *
 * \param surface The surface to copy from.
 * \param target Pointer to the target memory buffer.
 * \param size Size of the target buffer in bytes.
 * \param src_x X location on contents to copy from.
 * \param src_y Y location on contents to copy from.
 * \param width Width in pixels of the area to copy.
 * \param height Height in pixels of the area to copy.
 * \return 0 for success, -1 for failure.
 *
 * Surface contents are maintained by the renderer. They can be in a
 * reserved weston_buffer or as a copy, e.g. a GL texture, or something
 * else.
 *
 * Surface contents are copied into memory pointed to by target,
 * which has size bytes of space available. The target memory
 * may be larger than needed, but being smaller returns an error.
 * The extra bytes in target may or may not be written; their content is
 * unspecified. Size must be large enough to hold the image.
 *
 * The image in the target memory will be arranged in rows from
 * top to bottom, and pixels on a row from left to right. The pixel
 * format is PIXMAN_a8b8g8r8, 4 bytes per pixel, and stride is exactly
 * width * 4.
 *
 * Parameters src_x and src_y define the upper-left corner in buffer
 * coordinates (pixels) to copy from. Parameters width and height
 * define the size of the area to copy in pixels.
 *
 * The rectangle defined by src_x, src_y, width, height must fit in
 * the surface contents. Otherwise an error is returned.
 *
 * Use weston_surface_get_content_size to determine the content size; the
 * needed target buffer size and rectangle limits.
 *
 * CURRENT IMPLEMENTATION RESTRICTIONS:
 * - the machine must be little-endian due to Pixman formats.
 *
 * NOTE: Pixman formats are premultiplied.
 */
WL_EXPORT int
weston_surface_copy_content(struct weston_surface *surface,
			    void *target, size_t size,
			    int src_x, int src_y,
			    int width, int height)
{
	struct weston_renderer *rer = surface->compositor->renderer;
	int cw, ch;
	const size_t bytespp = 4; /* PIXMAN_a8b8g8r8 */

	if (!rer->surface_copy_content)
		return -1;

	weston_surface_get_content_size(surface, &cw, &ch);

	if (src_x < 0 || src_y < 0)
		return -1;

	if (width <= 0 || height <= 0)
		return -1;

	if (src_x + width > cw || src_y + height > ch)
		return -1;

	if (width * bytespp * height > size)
		return -1;

	return rer->surface_copy_content(surface, target, size,
					 src_x, src_y, width, height);
}

static void
subsurface_set_position(struct wl_client *client,
			struct wl_resource *resource, int32_t x, int32_t y)
{
	struct weston_subsurface *sub = wl_resource_get_user_data(resource);

	if (!sub)
		return;

	sub->position.x = x;
	sub->position.y = y;
	sub->position.set = 1;
}

static struct weston_subsurface *
subsurface_find_sibling(struct weston_subsurface *sub,
		       struct weston_surface *surface)
{
	struct weston_surface *parent = sub->parent;
	struct weston_subsurface *sibling;

	wl_list_for_each(sibling, &parent->subsurface_list, parent_link) {
		if (sibling->surface == surface && sibling != sub)
			return sibling;
	}

	return NULL;
}

static struct weston_subsurface *
subsurface_sibling_check(struct weston_subsurface *sub,
			 struct weston_surface *surface,
			 const char *request)
{
	struct weston_subsurface *sibling;

	sibling = subsurface_find_sibling(sub, surface);
	if (!sibling) {
		wl_resource_post_error(sub->resource,
			WL_SUBSURFACE_ERROR_BAD_SURFACE,
			"%s: wl_surface@%d is not a parent or sibling",
			request, wl_resource_get_id(surface->resource));
		return NULL;
	}

	assert(sibling->parent == sub->parent);

	return sibling;
}

static void
subsurface_place_above(struct wl_client *client,
		       struct wl_resource *resource,
		       struct wl_resource *sibling_resource)
{
	struct weston_subsurface *sub = wl_resource_get_user_data(resource);
	struct weston_surface *surface =
		wl_resource_get_user_data(sibling_resource);
	struct weston_subsurface *sibling;

	if (!sub)
		return;

	sibling = subsurface_sibling_check(sub, surface, "place_above");
	if (!sibling)
		return;

	wl_list_remove(&sub->parent_link_pending);
	wl_list_insert(sibling->parent_link_pending.prev,
		       &sub->parent_link_pending);

	sub->reordered = true;
}

static void
subsurface_place_below(struct wl_client *client,
		       struct wl_resource *resource,
		       struct wl_resource *sibling_resource)
{
	struct weston_subsurface *sub = wl_resource_get_user_data(resource);
	struct weston_surface *surface =
		wl_resource_get_user_data(sibling_resource);
	struct weston_subsurface *sibling;

	if (!sub)
		return;

	sibling = subsurface_sibling_check(sub, surface, "place_below");
	if (!sibling)
		return;

	wl_list_remove(&sub->parent_link_pending);
	wl_list_insert(&sibling->parent_link_pending,
		       &sub->parent_link_pending);

	sub->reordered = true;
}

static void
subsurface_set_sync(struct wl_client *client, struct wl_resource *resource)
{
	struct weston_subsurface *sub = wl_resource_get_user_data(resource);

	if (sub)
		sub->synchronized = 1;
}

static void
subsurface_set_desync(struct wl_client *client, struct wl_resource *resource)
{
	struct weston_subsurface *sub = wl_resource_get_user_data(resource);

	if (sub && sub->synchronized) {
		sub->synchronized = 0;

		/* If sub became effectively desynchronized, flush. */
		if (!weston_subsurface_is_synchronized(sub))
			weston_subsurface_synchronized_commit(sub);
	}
}

static void
weston_subsurface_unlink_parent(struct weston_subsurface *sub)
{
	wl_list_remove(&sub->parent_link);
	wl_list_remove(&sub->parent_link_pending);
	wl_list_remove(&sub->parent_destroy_listener.link);
	sub->parent = NULL;
}

static void
weston_subsurface_destroy(struct weston_subsurface *sub);

static void
subsurface_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct weston_subsurface *sub =
		container_of(listener, struct weston_subsurface,
			     surface_destroy_listener);
	assert(data == sub->surface);

	/* The protocol object (wl_resource) is left inert. */
	if (sub->resource)
		wl_resource_set_user_data(sub->resource, NULL);

	weston_subsurface_destroy(sub);
}

static void
subsurface_handle_parent_destroy(struct wl_listener *listener, void *data)
{
	struct weston_subsurface *sub =
		container_of(listener, struct weston_subsurface,
			     parent_destroy_listener);
	assert(data == sub->parent);
	assert(sub->surface != sub->parent);

	if (weston_surface_is_mapped(sub->surface))
		weston_surface_unmap(sub->surface);

	weston_subsurface_unlink_parent(sub);
}

static void
subsurface_resource_destroy(struct wl_resource *resource)
{
	struct weston_subsurface *sub = wl_resource_get_user_data(resource);

	if (sub)
		weston_subsurface_destroy(sub);
}

static void
subsurface_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
weston_subsurface_link_parent(struct weston_subsurface *sub,
			      struct weston_surface *parent)
{
	sub->parent = parent;
	sub->parent_destroy_listener.notify = subsurface_handle_parent_destroy;
	wl_signal_add(&parent->destroy_signal,
		      &sub->parent_destroy_listener);

	wl_list_insert(&parent->subsurface_list, &sub->parent_link);
	wl_list_insert(&parent->subsurface_list_pending,
		       &sub->parent_link_pending);
}

static void
weston_subsurface_link_surface(struct weston_subsurface *sub,
			       struct weston_surface *surface)
{
	sub->surface = surface;
	sub->surface_destroy_listener.notify =
		subsurface_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &sub->surface_destroy_listener);
}

static void
weston_subsurface_destroy(struct weston_subsurface *sub)
{
	struct weston_view *view, *next;

	assert(sub->surface);

	if (sub->resource) {
		assert(weston_surface_to_subsurface(sub->surface) == sub);
		assert(sub->parent_destroy_listener.notify ==
		       subsurface_handle_parent_destroy);

		wl_list_for_each_safe(view, next, &sub->surface->views, surface_link) {
			weston_view_unmap(view);
			weston_view_destroy(view);
		}

		if (sub->parent)
			weston_subsurface_unlink_parent(sub);

		weston_surface_state_fini(&sub->cached);
		weston_buffer_reference(&sub->cached_buffer_ref, NULL);

		sub->surface->committed = NULL;
		sub->surface->committed_private = NULL;
		weston_surface_set_label_func(sub->surface, NULL);
	} else {
		/* the dummy weston_subsurface for the parent itself */
		assert(sub->parent_destroy_listener.notify == NULL);
		wl_list_remove(&sub->parent_link);
		wl_list_remove(&sub->parent_link_pending);
	}

	wl_list_remove(&sub->surface_destroy_listener.link);
	free(sub);
}

static const struct wl_subsurface_interface subsurface_implementation = {
	subsurface_destroy,
	subsurface_set_position,
	subsurface_place_above,
	subsurface_place_below,
	subsurface_set_sync,
	subsurface_set_desync
};

static struct weston_subsurface *
weston_subsurface_create(uint32_t id, struct weston_surface *surface,
			 struct weston_surface *parent)
{
	struct weston_subsurface *sub;
	struct wl_client *client = wl_resource_get_client(surface->resource);

	sub = zalloc(sizeof *sub);
	if (sub == NULL)
		return NULL;

	wl_list_init(&sub->unused_views);

	sub->resource =
		wl_resource_create(client, &wl_subsurface_interface, 1, id);
	if (!sub->resource) {
		free(sub);
		return NULL;
	}

	wl_resource_set_implementation(sub->resource,
				       &subsurface_implementation,
				       sub, subsurface_resource_destroy);
	weston_subsurface_link_surface(sub, surface);
	weston_subsurface_link_parent(sub, parent);
	weston_surface_state_init(&sub->cached);
	sub->cached_buffer_ref.buffer = NULL;
	sub->synchronized = 1;

	return sub;
}

/* Create a dummy subsurface for having the parent itself in its
 * sub-surface lists. Makes stacking order manipulation easy.
 */
static struct weston_subsurface *
weston_subsurface_create_for_parent(struct weston_surface *parent)
{
	struct weston_subsurface *sub;

	sub = zalloc(sizeof *sub);
	if (sub == NULL)
		return NULL;

	weston_subsurface_link_surface(sub, parent);
	sub->parent = parent;
	wl_list_insert(&parent->subsurface_list, &sub->parent_link);
	wl_list_insert(&parent->subsurface_list_pending,
		       &sub->parent_link_pending);

	return sub;
}

static void
subcompositor_get_subsurface(struct wl_client *client,
			     struct wl_resource *resource,
			     uint32_t id,
			     struct wl_resource *surface_resource,
			     struct wl_resource *parent_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct weston_surface *parent =
		wl_resource_get_user_data(parent_resource);
	struct weston_subsurface *sub;
	static const char where[] = "get_subsurface: wl_subsurface@";

	if (surface == parent) {
		wl_resource_post_error(resource,
			WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
			"%s%d: wl_surface@%d cannot be its own parent",
			where, id, wl_resource_get_id(surface_resource));
		return;
	}

	if (weston_surface_to_subsurface(surface)) {
		wl_resource_post_error(resource,
			WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
			"%s%d: wl_surface@%d is already a sub-surface",
			where, id, wl_resource_get_id(surface_resource));
		return;
	}

	if (weston_surface_set_role(surface, "wl_subsurface", resource,
				    WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE) < 0)
		return;

	if (weston_surface_get_main_surface(parent) == surface) {
		wl_resource_post_error(resource,
			WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
			"%s%d: wl_surface@%d is an ancestor of parent",
			where, id, wl_resource_get_id(surface_resource));
		return;
	}

	/* make sure the parent is in its own list */
	if (wl_list_empty(&parent->subsurface_list)) {
		if (!weston_subsurface_create_for_parent(parent)) {
			wl_resource_post_no_memory(resource);
			return;
		}
	}

	sub = weston_subsurface_create(id, surface, parent);
	if (!sub) {
		wl_resource_post_no_memory(resource);
		return;
	}

	surface->committed = subsurface_committed;
	surface->committed_private = sub;
	weston_surface_set_label_func(surface, subsurface_get_label);
}

static void
subcompositor_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_subcompositor_interface subcompositor_interface = {
	subcompositor_destroy,
	subcompositor_get_subsurface
};

static void
bind_subcompositor(struct wl_client *client,
		   void *data, uint32_t version, uint32_t id)
{
	struct weston_compositor *compositor = data;
	struct wl_resource *resource;

	resource =
		wl_resource_create(client, &wl_subcompositor_interface, 1, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &subcompositor_interface,
				       compositor, NULL);
}

/** Set a DPMS mode on all of the compositor's outputs
 *
 * \param compositor The compositor instance
 * \param state The DPMS state the outputs will be set to
 */
static void
weston_compositor_dpms(struct weston_compositor *compositor,
		       enum dpms_enum state)
{
        struct weston_output *output;

        wl_list_for_each(output, &compositor->output_list, link)
		if (output->set_dpms)
			output->set_dpms(output, state);
}

/** Restores the compositor to active status
 *
 * \param compositor The compositor instance
 *
 * If the compositor was in a sleeping mode, all outputs are powered
 * back on via DPMS.  Otherwise if the compositor was inactive
 * (idle/locked, offscreen, or sleeping) then the compositor's wake
 * signal will fire.
 *
 * Restarts the idle timer.
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_wake(struct weston_compositor *compositor)
{
	uint32_t old_state = compositor->state;

	/* The state needs to be changed before emitting the wake
	 * signal because that may try to schedule a repaint which
	 * will not work if the compositor is still sleeping */
	compositor->state = WESTON_COMPOSITOR_ACTIVE;

	switch (old_state) {
	case WESTON_COMPOSITOR_SLEEPING:
	case WESTON_COMPOSITOR_IDLE:
	case WESTON_COMPOSITOR_OFFSCREEN:
		weston_compositor_dpms(compositor, WESTON_DPMS_ON);
		wl_signal_emit(&compositor->wake_signal, compositor);
		/* fall through */
	default:
		wl_event_source_timer_update(compositor->idle_source,
					     compositor->idle_time * 1000);
	}
}

/** Turns off rendering and frame events for the compositor.
 *
 * \param compositor The compositor instance
 *
 * This is used for example to prevent further rendering while the
 * compositor is shutting down.
 *
 * Stops the idle timer.
 *
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_offscreen(struct weston_compositor *compositor)
{
	switch (compositor->state) {
	case WESTON_COMPOSITOR_OFFSCREEN:
		return;
	case WESTON_COMPOSITOR_SLEEPING:
	default:
		compositor->state = WESTON_COMPOSITOR_OFFSCREEN;
		wl_event_source_timer_update(compositor->idle_source, 0);
	}
}

/** Powers down all attached output devices
 *
 * \param compositor The compositor instance
 *
 * Causes rendering to the outputs to cease, and no frame events to be
 * sent.  Only powers down the outputs if the compositor is not already
 * in sleep mode.
 *
 * Stops the idle timer.
 *
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_sleep(struct weston_compositor *compositor)
{
	if (compositor->state == WESTON_COMPOSITOR_SLEEPING)
		return;

	wl_event_source_timer_update(compositor->idle_source, 0);
	compositor->state = WESTON_COMPOSITOR_SLEEPING;
	weston_compositor_dpms(compositor, WESTON_DPMS_OFF);
}

/** Sets compositor to idle mode
 *
 * \param data The compositor instance
 *
 * This is called when the idle timer fires.  Once the compositor is in
 * idle mode it requires a wake action (e.g. via
 * weston_compositor_wake()) to restore it.  The compositor's
 * idle_signal will be triggered when the idle event occurs.
 *
 * Idleness can be inhibited by setting the compositor's idle_inhibit
 * property.
 */
static int
idle_handler(void *data)
{
	struct weston_compositor *compositor = data;

	if (compositor->idle_inhibit)
		return 1;

	compositor->state = WESTON_COMPOSITOR_IDLE;
	wl_signal_emit(&compositor->idle_signal, compositor);

	return 1;
}

WL_EXPORT void
weston_plane_init(struct weston_plane *plane,
			struct weston_compositor *ec,
			int32_t x, int32_t y)
{
	pixman_region32_init(&plane->damage);
	pixman_region32_init(&plane->clip);
	plane->x = x;
	plane->y = y;
	plane->compositor = ec;

	/* Init the link so that the call to wl_list_remove() when releasing
	 * the plane without ever stacking doesn't lead to a crash */
	wl_list_init(&plane->link);
}

WL_EXPORT void
weston_plane_release(struct weston_plane *plane)
{
	struct weston_view *view;

	pixman_region32_fini(&plane->damage);
	pixman_region32_fini(&plane->clip);

	wl_list_for_each(view, &plane->compositor->view_list, link) {
		if (view->plane == plane)
			view->plane = NULL;
	}

	wl_list_remove(&plane->link);
}

/** weston_compositor_stack_plane
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_stack_plane(struct weston_compositor *ec,
			      struct weston_plane *plane,
			      struct weston_plane *above)
{
	if (above)
		wl_list_insert(above->link.prev, &plane->link);
	else
		wl_list_insert(&ec->plane_list, &plane->link);
}

static void
output_release(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_output_interface output_interface = {
	output_release,
};


static void unbind_resource(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
bind_output(struct wl_client *client,
	    void *data, uint32_t version, uint32_t id)
{
	struct weston_head *head = data;
	struct weston_output *output = head->output;
	struct weston_mode *mode;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wl_output_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_list_insert(&head->resource_list, wl_resource_get_link(resource));
	wl_resource_set_implementation(resource, &output_interface, head,
				       unbind_resource);

	assert(output);
	wl_output_send_geometry(resource,
				output->x,
				output->y,
				head->mm_width,
				head->mm_height,
				head->subpixel,
				head->make, head->model,
				output->transform);
	if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
		wl_output_send_scale(resource,
				     output->current_scale);

	wl_list_for_each (mode, &output->mode_list, link) {
		wl_output_send_mode(resource,
				    mode->flags,
				    mode->width,
				    mode->height,
				    mode->refresh);
	}

	if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
		wl_output_send_done(resource);
}

static void
weston_head_add_global(struct weston_head *head)
{
	head->global = wl_global_create(head->compositor->wl_display,
					&wl_output_interface, 3,
					head, bind_output);
}

/** Remove the global wl_output protocol object
 *
 * \param head The head whose global to remove.
 *
 * Also orphans the wl_resources for this head (wl_output).
 */
static void
weston_head_remove_global(struct weston_head *head)
{
	struct wl_resource *resource, *tmp;

	if (head->global)
		wl_global_destroy(head->global);
	head->global = NULL;

	wl_resource_for_each_safe(resource, tmp, &head->resource_list) {
		unbind_resource(resource);
		wl_resource_set_destructor(resource, NULL);
		wl_resource_set_user_data(resource, NULL);
	}

	wl_resource_for_each(resource, &head->xdg_output_resource_list) {
		/* It's sufficient to unset the destructor, then the list elements
		 * won't be accessed.
		 */
		wl_resource_set_destructor(resource, NULL);
	}
	wl_list_init(&head->xdg_output_resource_list);
}

/** Get the backing object of wl_output
 *
 * \param resource A wl_output protocol object.
 * \return The backing object (user data) of a wl_resource representing a
 * wl_output protocol object.
 *
 * \ingroup head
 */
WL_EXPORT struct weston_head *
weston_head_from_resource(struct wl_resource *resource)
{
	assert(wl_resource_instance_of(resource, &wl_output_interface,
				       &output_interface));

	return wl_resource_get_user_data(resource);
}

/** Initialize a pre-allocated weston_head
 *
 * \param head The head to initialize.
 * \param name The head name, e.g. the connector name or equivalent.
 *
 * The head will be safe to attach, detach and release.
 *
 * The name is used in logs, and can be used by compositors as a configuration
 * identifier.
 *
 * \ingroup head
 * \internal
 */
WL_EXPORT void
weston_head_init(struct weston_head *head, const char *name)
{
	/* Add some (in)sane defaults which can be used
	 * for checking if an output was properly configured
	 */
	memset(head, 0, sizeof *head);

	wl_list_init(&head->compositor_link);
	wl_signal_init(&head->destroy_signal);
	wl_list_init(&head->output_link);
	wl_list_init(&head->resource_list);
	wl_list_init(&head->xdg_output_resource_list);
	head->name = strdup(name);
	head->current_protection = WESTON_HDCP_DISABLE;
}

/** Send output heads changed signal
 *
 * \param output The output that changed.
 *
 * Notify that the enabled output gained and/or lost heads, or that the
 * associated heads may have changed their connection status. This does not
 * include cases where the output becomes enabled or disabled. The registered
 * callbacks are called after the change has successfully happened.
 *
 * If connection status change causes the compositor to attach or detach a head
 * to an enabled output, the registered callbacks may be called multiple times.
 *
 * \ingroup output
 */
static void
weston_output_emit_heads_changed(struct weston_output *output)
{
	wl_signal_emit(&output->compositor->output_heads_changed_signal,
		       output);
}

/** Idle task for emitting heads_changed_signal */
static void
weston_compositor_call_heads_changed(void *data)
{
	struct weston_compositor *compositor = data;
	struct weston_head *head;

	compositor->heads_changed_source = NULL;

	wl_signal_emit(&compositor->heads_changed_signal, compositor);

	wl_list_for_each(head, &compositor->head_list, compositor_link) {
		if (head->output && head->output->enabled)
			weston_output_emit_heads_changed(head->output);
	}
}

/** Schedule a call on idle to heads_changed callback
 *
 * \param compositor The Compositor.
 *
 * \ingroup compositor
 * \internal
 */
static void
weston_compositor_schedule_heads_changed(struct weston_compositor *compositor)
{
	struct wl_event_loop *loop;

	if (compositor->heads_changed_source)
		return;

	loop = wl_display_get_event_loop(compositor->wl_display);
	compositor->heads_changed_source = wl_event_loop_add_idle(loop,
			weston_compositor_call_heads_changed, compositor);
}

/** Register a new head
 *
 * \param compositor The compositor.
 * \param head The head to register, must not be already registered.
 *
 * This signals the core that a new head has become available, leading to
 * heads_changed hook being called later.
 *
 * \ingroup compositor
 * \internal
 */
WL_EXPORT void
weston_compositor_add_head(struct weston_compositor *compositor,
			   struct weston_head *head)
{
	assert(wl_list_empty(&head->compositor_link));
	assert(head->name);

	wl_list_insert(compositor->head_list.prev, &head->compositor_link);
	head->compositor = compositor;
	weston_compositor_schedule_heads_changed(compositor);
}

/** Adds a listener to be called when heads change
 *
 * \param compositor The compositor.
 * \param listener The listener to add.
 *
 * The listener notify function argument is weston_compositor.
 *
 * The listener function will be called after heads are added or their
 * connection status has changed. Several changes may be accumulated into a
 * single call. The user is expected to iterate over the existing heads and
 * check their statuses to find out what changed.
 *
 * \sa weston_compositor_iterate_heads, weston_head_is_connected,
 * weston_head_is_enabled
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_add_heads_changed_listener(struct weston_compositor *compositor,
					     struct wl_listener *listener)
{
	wl_signal_add(&compositor->heads_changed_signal, listener);
}

/** Iterate over available heads
 *
 * \param compositor The compositor.
 * \param iter The iterator, or NULL for start.
 * \return The next available head in the list.
 *
 * Returns all available heads, regardless of being connected or enabled.
 *
 * You can iterate over all heads as follows:
 * \code
 * struct weston_head *head = NULL;
 *
 * while ((head = weston_compositor_iterate_heads(compositor, head))) {
 * 	...
 * }
 * \endcode
 *
 *  If you cause \c iter to be removed from the list, you cannot use it to
 * continue iterating. Removing any other item is safe.
 *
 * \ingroup compositor
 */
WL_EXPORT struct weston_head *
weston_compositor_iterate_heads(struct weston_compositor *compositor,
				struct weston_head *iter)
{
	struct wl_list *list = &compositor->head_list;
	struct wl_list *node;

	assert(compositor);
	assert(!iter || iter->compositor == compositor);

	if (iter)
		node = iter->compositor_link.next;
	else
		node = list->next;

	assert(node);
	assert(!iter || node != &iter->compositor_link);

	if (node == list)
		return NULL;

	return container_of(node, struct weston_head, compositor_link);
}

/** Iterate over attached heads
 *
 * \param output The output whose heads to iterate.
 * \param iter The iterator, or NULL for start.
 * \return The next attached head in the list.
 *
 * Returns all heads currently attached to the output.
 *
 * You can iterate over all heads as follows:
 * \code
 * struct weston_head *head = NULL;
 *
 * while ((head = weston_output_iterate_heads(output, head))) {
 * 	...
 * }
 * \endcode
 *
 *  If you cause \c iter to be removed from the list, you cannot use it to
 * continue iterating. Removing any other item is safe.
 *
 * \ingroup ouput
 */
WL_EXPORT struct weston_head *
weston_output_iterate_heads(struct weston_output *output,
			    struct weston_head *iter)
{
	struct wl_list *list = &output->head_list;
	struct wl_list *node;

	assert(output);
	assert(!iter || iter->output == output);

	if (iter)
		node = iter->output_link.next;
	else
		node = list->next;

	assert(node);
	assert(!iter || node != &iter->output_link);

	if (node == list)
		return NULL;

	return container_of(node, struct weston_head, output_link);
}

/** Attach a head to an output
 *
 * \param output The output to attach to.
 * \param head The head that is not yet attached.
 * \return 0 on success, -1 on failure.
 *
 * Attaches the given head to the output. All heads of an output are clones
 * and share the resolution and timings.
 *
 * Cloning heads this way uses less resources than creating an output for
 * each head, but is not always possible due to environment, driver and hardware
 * limitations.
 *
 * On failure, the head remains unattached. Success of this function does not
 * guarantee the output configuration is actually valid. The final checks are
 * made on weston_output_enable() unless the output was already enabled.
 *
 * \ingroup output
 */
WL_EXPORT int
weston_output_attach_head(struct weston_output *output,
			  struct weston_head *head)
{
	char *head_names;

	if (!wl_list_empty(&head->output_link))
		return -1;

	if (output->attach_head) {
		if (output->attach_head(output, head) < 0)
			return -1;
	} else if (!wl_list_empty(&output->head_list)) {
		/* No support for clones in the legacy path. */
		return -1;
	}

	head->output = output;
	wl_list_insert(output->head_list.prev, &head->output_link);

	if (output->enabled) {
		weston_head_add_global(head);

		head_names = weston_output_create_heads_string(output);
		weston_log("Output '%s' updated to have head(s) %s\n",
			   output->name, head_names);
		free(head_names);

		weston_output_emit_heads_changed(output);
	}

	return 0;
}

/** Detach a head from its output
 *
 * \param head The head to detach.
 *
 * It is safe to detach a non-attached head.
 *
 * If the head is attached to an enabled output and the output will be left
 * with no heads, the output will be disabled.
 *
 * \ingroup head
 * \sa weston_output_disable
 */
WL_EXPORT void
weston_head_detach(struct weston_head *head)
{
	struct weston_output *output = head->output;
	char *head_names;

	wl_list_remove(&head->output_link);
	wl_list_init(&head->output_link);
	head->output = NULL;

	if (!output)
		return;

	if (output->detach_head)
		output->detach_head(output, head);

	if (output->enabled) {
		weston_head_remove_global(head);

		if (wl_list_empty(&output->head_list)) {
			weston_log("Output '%s' no heads left, disabling.\n",
				   output->name);
			weston_output_disable(output);
		} else {
			head_names = weston_output_create_heads_string(output);
			weston_log("Output '%s' updated to have head(s) %s\n",
				   output->name, head_names);
			free(head_names);

			weston_output_emit_heads_changed(output);
		}
	}
}

/** Destroy a head
 *
 * \param head The head to be released.
 *
 * Destroys the head. The caller is responsible for freeing the memory pointed
 * to by \c head.
 *
 * \ingroup head
 * \internal
 */
WL_EXPORT void
weston_head_release(struct weston_head *head)
{
	wl_signal_emit(&head->destroy_signal, head);

	weston_head_detach(head);

	free(head->make);
	free(head->model);
	free(head->serial_number);
	free(head->name);

	wl_list_remove(&head->compositor_link);
}

static void
weston_head_set_device_changed(struct weston_head *head)
{
	head->device_changed = true;

	if (head->compositor)
		weston_compositor_schedule_heads_changed(head->compositor);
}

/** String equal comparison with NULLs being equal */
static bool
str_null_eq(const char *a, const char *b)
{
	if (!a && !b)
		return true;

	if (!!a != !!b)
		return false;

	return strcmp(a, b) == 0;
}

/** Store monitor make, model and serial number
 *
 * \param head The head to modify.
 * \param make The monitor make. If EDID is available, the PNP ID. Otherwise
 * any string, or NULL for none.
 * \param model The monitor model or name, or a made-up string, or NULL for
 * none.
 * \param serialno The monitor serial number, a made-up string, or NULL for
 * none.
 *
 * This may set the device_changed flag.
 *
 * \ingroup head
 * \internal
 */
WL_EXPORT void
weston_head_set_monitor_strings(struct weston_head *head,
				const char *make,
				const char *model,
				const char *serialno)
{
	if (str_null_eq(head->make, make) &&
	    str_null_eq(head->model, model) &&
	    str_null_eq(head->serial_number, serialno))
		return;

	free(head->make);
	free(head->model);
	free(head->serial_number);

	head->make = make ? strdup(make) : NULL;
	head->model = model ? strdup(model) : NULL;
	head->serial_number = serialno ? strdup(serialno) : NULL;

	weston_head_set_device_changed(head);
}

/** Store display non-desktop status
 *
 * \param head The head to modify.
 * \param non_desktop Whether the head connects to a non-desktop display.
 *
 * \ingroup head
 * \internal
 */
WL_EXPORT void
weston_head_set_non_desktop(struct weston_head *head, bool non_desktop)
{
	if (head->non_desktop == non_desktop)
		return;

	head->non_desktop = non_desktop;

	weston_head_set_device_changed(head);
}

/** Store display transformation
 *
 * \param head The head to modify.
 * \param transform The transformation to apply for this head
 *
 * This may set the device_changed flag.
 *
 * \ingroup head
 * \internal
 */
WL_EXPORT void
weston_head_set_transform(struct weston_head *head, uint32_t transform)
{
	if (head->transform == transform)
		return;

	head->transform = transform;

	weston_head_set_device_changed(head);
}


/** Store physical image size
 *
 * \param head The head to modify.
 * \param mm_width Image area width in millimeters.
 * \param mm_height Image area height in millimeters.
 *
 * This may set the device_changed flag.
 *
 * \ingroup head
 * \internal
 */
WL_EXPORT void
weston_head_set_physical_size(struct weston_head *head,
			      int32_t mm_width, int32_t mm_height)
{
	if (head->mm_width == mm_width &&
	    head->mm_height == mm_height)
		return;

	head->mm_width = mm_width;
	head->mm_height = mm_height;

	weston_head_set_device_changed(head);
}

/** Store monitor sub-pixel layout
 *
 * \param head The head to modify.
 * \param sp Sub-pixel layout. The possible values are:
 * - WL_OUTPUT_SUBPIXEL_UNKNOWN,
 * - WL_OUTPUT_SUBPIXEL_NONE,
 * - WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB,
 * - WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR,
 * - WL_OUTPUT_SUBPIXEL_VERTICAL_RGB,
 * - WL_OUTPUT_SUBPIXEL_VERTICAL_BGR
 *
 * This may set the device_changed flag.
 *
 * \ingroup head
 * \internal
 */
WL_EXPORT void
weston_head_set_subpixel(struct weston_head *head,
			 enum wl_output_subpixel sp)
{
	if (head->subpixel == sp)
		return;

	head->subpixel = sp;

	weston_head_set_device_changed(head);
}

/** Mark the monitor as internal
 *
 * This is used for embedded screens, like laptop panels.
 *
 * \param head The head to mark as internal.
 *
 * By default a head is external. The type is often inferred from the physical
 * connector type.
 *
 * \ingroup head
 * \internal
 */
WL_EXPORT void
weston_head_set_internal(struct weston_head *head)
{
	head->connection_internal = true;
}

/** Store connector status
 *
 * \param head The head to modify.
 * \param connected Whether the head is connected.
 *
 * Connectors are created as disconnected. This function can be used to
 * set the connector status.
 *
 * The status should be set to true when a physical connector is connected to
 * a video sink device like a monitor and to false when the connector is
 * disconnected. For nested backends, the connection status should reflect the
 * connection to the parent display server.
 *
 * When the connection status changes, it schedules a call to the heads_changed
 * hook and sets the device_changed flag.
 *
 * \sa weston_compositor_set_heads_changed_cb
 * \ingroup head
 * \internal
 */
WL_EXPORT void
weston_head_set_connection_status(struct weston_head *head, bool connected)
{
	if (head->connected == connected)
		return;

	head->connected = connected;

	weston_head_set_device_changed(head);
}

static void
weston_output_compute_protection(struct weston_output *output)
{
	struct weston_head *head;
	enum weston_hdcp_protection op_protection;
	bool op_protection_valid = false;
	struct weston_compositor *wc = output->compositor;

	wl_list_for_each(head, &output->head_list, output_link) {
		if (!op_protection_valid) {
			op_protection = head->current_protection;
			op_protection_valid = true;
		}
		if (head->current_protection < op_protection)
			op_protection = head->current_protection;
	}

	if (!op_protection_valid)
		op_protection = WESTON_HDCP_DISABLE;

	if (output->current_protection != op_protection) {
		output->current_protection = op_protection;
		weston_output_damage(output);
		weston_schedule_surface_protection_update(wc);
	}
}

WL_EXPORT void
weston_head_set_content_protection_status(struct weston_head *head,
					  enum weston_hdcp_protection status)
{
	head->current_protection = status;
	if (head->output)
		weston_output_compute_protection(head->output);
}

/** Is the head currently connected?
 *
 * \param head The head to query.
 * \return Connection status.
 *
 * Returns true if the head is physically connected to a monitor, or in
 * case of a nested backend returns true when there is a connection to the
 * parent display server.
 *
 * This is independent from the head being enabled.
 *
 * \sa weston_head_is_enabled
 * \ingroup head
 */
WL_EXPORT bool
weston_head_is_connected(struct weston_head *head)
{
	return head->connected;
}

/** Is the head currently enabled?
 *
 * \param head The head to query.
 * \return Video status.
 *
 * Returns true if the head is currently transmitting a video stream.
 *
 * This is independent of the head being connected.
 *
 * \sa weston_head_is_connected
 * \ingroup head
 */
WL_EXPORT bool
weston_head_is_enabled(struct weston_head *head)
{
	if (!head->output)
		return false;

	return head->output->enabled;
}

/** Has the device information changed?
 *
 * \param head The head to query.
 * \return True if the device information has changed since last reset.
 *
 * The information about the connected display device, e.g. a monitor, may
 * change without being disconnected in between. Changing information
 * causes a call to the heads_changed hook.
 *
 * The information includes make, model, serial number, physical size,
 * and sub-pixel type. The connection status is also included.
 *
 * \sa weston_head_reset_device_changed, weston_compositor_set_heads_changed_cb
 * \ingroup head
 */
WL_EXPORT bool
weston_head_is_device_changed(struct weston_head *head)
{
	return head->device_changed;
}

/** Does the head represent a non-desktop display?
 *
 * \param head The head to query.
 * \return True if the device is a non-desktop display.
 *
 * Non-desktop heads are not attached to outputs by default.
 * This stops weston from extending the desktop onto head mounted displays.
 *
 * \ingroup head
 */
WL_EXPORT bool
weston_head_is_non_desktop(struct weston_head *head)
{
	return head->non_desktop;
}

/** Acknowledge device information change
 *
 * \param head The head to acknowledge.
 *
 * Clears the device changed flag on this head. When a compositor has processed
 * device information, it should call this to be able to notice further
 * changes.
 *
 * \sa weston_head_is_device_changed
 * \ingroup head
 */
WL_EXPORT void
weston_head_reset_device_changed(struct weston_head *head)
{
	head->device_changed = false;
}

/** Get the name of a head
 *
 * \param head The head to query.
 * \return The head's name, not NULL.
 *
 * The name depends on the backend. The DRM backend uses connector names,
 * other backends may use hardcoded names or user-given names.
 *
 * \ingroup head
 */
WL_EXPORT const char *
weston_head_get_name(struct weston_head *head)
{
	return head->name;
}

/** Get the output the head is attached to
 *
 * \param head The head to query.
 * \return The output the head is attached to, or NULL if detached.
 * \ingroup head
 */
WL_EXPORT struct weston_output *
weston_head_get_output(struct weston_head *head)
{
	return head->output;
}

/** Get the head's native transformation
 *
 * \param head The head to query.
 * \return The head's native transform, as a WL_OUTPUT_TRANSFORM_* value
 *
 * A weston_head may have a 'native' transform provided by the backend.
 * Examples include panels which are physically rotated, where the rotation
 * is recorded and described as part of the system configuration. This call
 * will return any known native transform for the head.
 *
 * \ingroup head
 */
WL_EXPORT uint32_t
weston_head_get_transform(struct weston_head *head)
{
	return head->transform;
}

/** Add destroy callback for a head
 *
 * \param head The head to watch for.
 * \param listener The listener to add. The \c notify member must be set.
 *
 * Heads may get destroyed for various reasons by the backends. If a head is
 * attached to an output, the compositor should listen for head destruction
 * and reconfigure or destroy the output if necessary.
 *
 * The destroy callbacks will be called on weston_head destruction before any
 * automatic detaching from an associated weston_output and before any
 * weston_head information is lost.
 *
 * The \c data argument to the notify callback is the weston_head being
 * destroyed.
 *
 * \ingroup head
 */
WL_EXPORT void
weston_head_add_destroy_listener(struct weston_head *head,
				 struct wl_listener *listener)
{
	wl_signal_add(&head->destroy_signal, listener);
}

/** Look up destroy listener for a head
 *
 * \param head The head to query.
 * \param notify The notify function used used for the added destroy listener.
 * \return The listener, or NULL if not found.
 *
 * This looks up the previously added destroy listener struct based on the
 * notify function it has. The listener can be used to access user data
 * through \c container_of().
 *
 * \sa wl_signal_get()
 * \ingroup head
 */
WL_EXPORT struct wl_listener *
weston_head_get_destroy_listener(struct weston_head *head,
				 wl_notify_func_t notify)
{
	return wl_signal_get(&head->destroy_signal, notify);
}

/* Move other outputs when one is resized so the space remains contiguous. */
static void
weston_compositor_reflow_outputs(struct weston_compositor *compositor,
				struct weston_output *resized_output, int delta_width)
{
	struct weston_output *output;
	bool start_resizing = false;

	if (!delta_width)
		return;

	wl_list_for_each(output, &compositor->output_list, link) {
		if (output == resized_output) {
			start_resizing = true;
			continue;
		}

		if (start_resizing) {
			weston_output_move(output, output->x + delta_width, output->y);
			output->dirty = 1;
		}
	}
}

static void
weston_output_update_matrix(struct weston_output *output)
{
	float magnification;

	weston_matrix_init(&output->matrix);
	weston_matrix_translate(&output->matrix, -output->x, -output->y, 0);

	if (output->zoom.active) {
		magnification = 1 / (1 - output->zoom.spring_z.current);
		weston_output_update_zoom(output);
		weston_matrix_translate(&output->matrix, -output->zoom.trans_x,
					-output->zoom.trans_y, 0);
		weston_matrix_scale(&output->matrix, magnification,
				    magnification, 1.0);
	}

	switch (output->transform) {
	case WL_OUTPUT_TRANSFORM_FLIPPED:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		weston_matrix_translate(&output->matrix, -output->width, 0, 0);
		weston_matrix_scale(&output->matrix, -1, 1, 1);
		break;
	}

	switch (output->transform) {
	default:
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		break;
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		weston_matrix_translate(&output->matrix, -output->width, 0, 0);
		weston_matrix_rotate_xy(&output->matrix, 0, -1);
		break;
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		weston_matrix_translate(&output->matrix,
					-output->width, -output->height, 0);
		weston_matrix_rotate_xy(&output->matrix, -1, 0);
		break;
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		weston_matrix_translate(&output->matrix, 0, -output->height, 0);
		weston_matrix_rotate_xy(&output->matrix, 0, 1);
		break;
	}

	if (output->current_scale != 1)
		weston_matrix_scale(&output->matrix,
				    output->current_scale,
				    output->current_scale, 1);

	output->dirty = 0;

	weston_matrix_invert(&output->inverse_matrix, &output->matrix);
}

static void
weston_output_transform_scale_init(struct weston_output *output, uint32_t transform, uint32_t scale)
{
	output->transform = transform;
	output->native_scale = scale;
	output->current_scale = scale;

	convert_size_by_transform_scale(&output->width, &output->height,
					output->current_mode->width,
					output->current_mode->height,
					transform, scale);
}

static void
weston_output_init_geometry(struct weston_output *output, int x, int y)
{
	output->x = x;
	output->y = y;

	pixman_region32_fini(&output->region);
	pixman_region32_init_rect(&output->region, x, y,
				  output->width,
				  output->height);
}

/**
 * \ingroup output
 */
WL_EXPORT void
weston_output_move(struct weston_output *output, int x, int y)
{
	struct weston_head *head;
	struct wl_resource *resource;
	int ver;

	output->move_x = x - output->x;
	output->move_y = y - output->y;

	if (output->move_x == 0 && output->move_y == 0)
		return;

	weston_output_init_geometry(output, x, y);

	output->dirty = 1;

	/* Move views on this output. */
	wl_signal_emit(&output->compositor->output_moved_signal, output);

	/* Notify clients of the change for output position. */
	wl_list_for_each(head, &output->head_list, output_link) {
		wl_resource_for_each(resource, &head->resource_list) {
			wl_output_send_geometry(resource,
						output->x,
						output->y,
						head->mm_width,
						head->mm_height,
						head->subpixel,
						head->make,
						head->model,
						output->transform);

			ver = wl_resource_get_version(resource);
			if (ver >= WL_OUTPUT_DONE_SINCE_VERSION)
				wl_output_send_done(resource);
		}

		wl_resource_for_each(resource, &head->xdg_output_resource_list) {
			zxdg_output_v1_send_logical_position(resource,
							     output->x,
							     output->y);
			zxdg_output_v1_send_done(resource);
		}
	}
}

/** Signal that a pending output is taken into use.
 *
 * Removes the output from the pending list and adds it to the compositor's
 * list of enabled outputs. The output created signal is emitted.
 *
 * The output gets an internal ID assigned, and the wl_output global is
 * created.
 *
 * \param compositor The compositor instance.
 * \param output The output to be added.
 *
 * \internal
 * \ingroup compositor
 */
static void
weston_compositor_add_output(struct weston_compositor *compositor,
                             struct weston_output *output)
{
	struct weston_view *view, *next;
	struct weston_head *head;

	assert(!output->enabled);

	/* Verify we haven't reached the limit of 32 available output IDs */
	assert(ffs(~compositor->output_id_pool) > 0);

	/* Invert the output id pool and look for the lowest numbered
	 * switch (the least significant bit).  Take that bit's position
	 * as our ID, and mark it used in the compositor's output_id_pool.
	 */
	output->id = ffs(~compositor->output_id_pool) - 1;
	compositor->output_id_pool |= 1u << output->id;

	wl_list_remove(&output->link);
	wl_list_insert(compositor->output_list.prev, &output->link);
	output->enabled = true;

	wl_list_for_each(head, &output->head_list, output_link)
		weston_head_add_global(head);

	wl_signal_emit(&compositor->output_created_signal, output);

	wl_list_for_each_safe(view, next, &compositor->view_list, link)
		weston_view_geometry_dirty(view);
}

/** Transform device coordinates into global coordinates
 *
 * \param output the weston_output object
 * \param[in] device_x X coordinate in device units.
 * \param[in] device_y Y coordinate in device units.
 * \param[out] x X coordinate in the global space.
 * \param[out] y Y coordinate in the global space.
 *
 * Transforms coordinates from the device coordinate space (physical pixel
 * units) to the global coordinate space (logical pixel units).  This takes
 * into account output transform and scale.
 *
 * \ingroup output
 * \internal
 */
WL_EXPORT void
weston_output_transform_coordinate(struct weston_output *output,
				   double device_x, double device_y,
				   double *x, double *y)
{
	struct weston_vector p = { {
		device_x,
		device_y,
		0.0,
		1.0 } };

	weston_matrix_transform(&output->inverse_matrix, &p);

	*x = p.f[0] / p.f[3];
	*y = p.f[1] / p.f[3];
}

/** Removes output from compositor's list of enabled outputs
 *
 * \param output The weston_output object that is being removed.
 *
 * The following happens:
 *
 * - The output assignments of all views in the current scenegraph are
 *   recomputed.
 *
 * - Presentation feedback is discarded.
 *
 * - Compositor is notified that outputs were changed and
 *   applies the necessary changes to re-layout outputs.
 *
 * - The output is put back in the pending outputs list.
 *
 * - Signal is emitted to notify all users of the weston_output
 *   object that the output is being destroyed.
 *
 * - wl_output protocol objects referencing this weston_output
 *   are made inert, and the wl_output global is removed.
 *
 * - The output's internal ID is released.
 *
 * \ingroup compositor
 * \internal
 */
static void
weston_compositor_remove_output(struct weston_output *output)
{
	struct weston_compositor *compositor = output->compositor;
	struct weston_view *view;
	struct weston_head *head;

	assert(output->destroying);
	assert(output->enabled);

	wl_list_for_each(view, &compositor->view_list, link) {
		if (view->output_mask & (1u << output->id))
			weston_view_assign_output(view);
	}

	weston_presentation_feedback_discard_list(&output->feedback_list);

	weston_compositor_reflow_outputs(compositor, output, -output->width);

	wl_list_remove(&output->link);
	wl_list_insert(compositor->pending_output_list.prev, &output->link);
	output->enabled = false;

	wl_signal_emit(&compositor->output_destroyed_signal, output);
	wl_signal_emit(&output->destroy_signal, output);

	wl_list_for_each(head, &output->head_list, output_link)
		weston_head_remove_global(head);

	compositor->output_id_pool &= ~(1u << output->id);
	output->id = 0xffffffff; /* invalid */
}

/** Sets the output scale for a given output.
 *
 * \param output The weston_output object that the scale is set for.
 * \param scale  Scale factor for the given output.
 *
 * It only supports setting scale for an output that
 * is not enabled and it can only be ran once.
 *
 * \ingroup ouput
 */
WL_EXPORT void
weston_output_set_scale(struct weston_output *output,
			int32_t scale)
{
	/* We can only set scale on a disabled output */
	assert(!output->enabled);

	/* We only want to set scale once */
	assert(!output->scale);

	output->scale = scale;
}

/** Sets the output transform for a given output.
 *
 * \param output    The weston_output object that the transform is set for.
 * \param transform Transform value for the given output.
 *
 * Refer to wl_output::transform section located at
 * https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_output
 * for list of values that can be passed to this function.
 *
 * \ingroup output
 */
WL_EXPORT void
weston_output_set_transform(struct weston_output *output,
			    uint32_t transform)
{
	struct weston_pointer_motion_event ev;
	struct wl_resource *resource;
	struct weston_seat *seat;
	pixman_region32_t old_region;
	int mid_x, mid_y;
	struct weston_head *head;
	int ver;

	if (!output->enabled && output->transform == UINT32_MAX) {
		output->transform = transform;
		return;
	}

	weston_output_transform_scale_init(output, transform, output->scale);

	pixman_region32_init(&old_region);
	pixman_region32_copy(&old_region, &output->region);

	weston_output_init_geometry(output, output->x, output->y);

	output->dirty = 1;

	/* Notify clients of the change for output transform. */
	wl_list_for_each(head, &output->head_list, output_link) {
		wl_resource_for_each(resource, &head->resource_list) {
			wl_output_send_geometry(resource,
						output->x,
						output->y,
						head->mm_width,
						head->mm_height,
						head->subpixel,
						head->make,
						head->model,
						output->transform);

			ver = wl_resource_get_version(resource);
			if (ver >= WL_OUTPUT_DONE_SINCE_VERSION)
				wl_output_send_done(resource);
		}
		wl_resource_for_each(resource, &head->xdg_output_resource_list) {
			zxdg_output_v1_send_logical_position(resource,
							     output->x,
							     output->y);
			zxdg_output_v1_send_logical_size(resource,
							 output->width,
							 output->height);
			zxdg_output_v1_send_done(resource);
		}
	}

	/* we must ensure that pointers are inside output, otherwise they disappear */
	mid_x = output->x + output->width / 2;
	mid_y = output->y + output->height / 2;

	ev.mask = WESTON_POINTER_MOTION_ABS;
	ev.x = wl_fixed_to_double(wl_fixed_from_int(mid_x));
	ev.y = wl_fixed_to_double(wl_fixed_from_int(mid_y));

	wl_list_for_each(seat, &output->compositor->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);

		if (pointer && pixman_region32_contains_point(&old_region,
							      wl_fixed_to_int(pointer->x),
							      wl_fixed_to_int(pointer->y),
							      NULL))
			weston_pointer_move(pointer, &ev);
	}
}

/** Initializes a weston_output object with enough data so
 ** an output can be configured.
 *
 * \param output     The weston_output object to initialize
 * \param compositor The compositor instance.
 * \param name       Name for the output (the string is copied).
 *
 * Sets initial values for fields that are expected to be
 * configured either by compositors or backends.
 *
 * The name is used in logs, and can be used by compositors as a configuration
 * identifier.
 *
 * \ingroup output
 * \internal
 */
WL_EXPORT void
weston_output_init(struct weston_output *output,
		   struct weston_compositor *compositor,
		   const char *name)
{
	output->compositor = compositor;
	output->destroying = 0;
	output->name = strdup(name);
	wl_list_init(&output->link);
	wl_signal_init(&output->user_destroy_signal);
	output->enabled = false;
	output->desired_protection = WESTON_HDCP_DISABLE;
	output->allow_protection = true;

	wl_list_init(&output->head_list);

	/* Add some (in)sane defaults which can be used
	 * for checking if an output was properly configured
	 */
	output->scale = 0;
	/* Can't use -1 on uint32_t and 0 is valid enum value */
	output->transform = UINT32_MAX;

	pixman_region32_init(&output->region);
	wl_list_init(&output->mode_list);
}

/** Adds weston_output object to pending output list.
 *
 * \param output     The weston_output object to add
 * \param compositor The compositor instance.
 *
 * The opposite of this operation is built into weston_output_release().
 *
 * \ingroup compositor
 * \internal
 */
WL_EXPORT void
weston_compositor_add_pending_output(struct weston_output *output,
				     struct weston_compositor *compositor)
{
	assert(output->disable);
	assert(output->enable);

	wl_list_remove(&output->link);
	wl_list_insert(compositor->pending_output_list.prev, &output->link);
}

/** Create a string with the attached heads' names.
 *
 * The string must be free()'d.
 *
 * \ingroup output
 */
static char *
weston_output_create_heads_string(struct weston_output *output)
{
	FILE *fp;
	char *str = NULL;
	size_t size = 0;
	struct weston_head *head;
	const char *sep = "";

	fp = open_memstream(&str, &size);
	if (!fp)
		return NULL;

	wl_list_for_each(head, &output->head_list, output_link) {
		fprintf(fp, "%s%s", sep, head->name);
		sep = ", ";
	}
	fclose(fp);

	return str;
}

/** Constructs a weston_output object that can be used by the compositor.
 *
 * \param output The weston_output object that needs to be enabled. Must not
 * be enabled already. Must have at least one head attached.
 *
 * Output coordinates are calculated and each new output is by default
 * assigned to the right of previous one.
 *
 * Sets up the transformation, zoom, and geometry of the output using
 * the properties that need to be configured by the compositor.
 *
 * Establishes a repaint timer for the output with the relevant display
 * object's event loop. See output_repaint_timer_handler().
 *
 * The output is assigned an ID. Weston can support up to 32 distinct
 * outputs, with IDs numbered from 0-31; the compositor's output_id_pool
 * is referred to and used to find the first available ID number, and
 * then this ID is marked as used in output_id_pool.
 *
 * The output is also assigned a Wayland global with the wl_output
 * external interface.
 *
 * Backend specific function is called to set up the output output.
 *
 * Output is added to the compositor's output list
 *
 * If the backend specific function fails, the weston_output object
 * is returned to a state it was before calling this function and
 * is added to the compositor's pending_output_list in case it needs
 * to be reconfigured or just so it can be destroyed at shutdown.
 *
 * 0 is returned on success, -1 on failure.
 *
 * \ingroup output
 */
WL_EXPORT int
weston_output_enable(struct weston_output *output)
{
	struct weston_compositor *c = output->compositor;
	struct weston_output *iterator;
	struct weston_head *head;
	char *head_names;
	int x = 0, y = 0;

	if (output->enabled) {
		weston_log("Error: attempt to enable an enabled output '%s'\n",
			   output->name);
		return -1;
	}

	if (wl_list_empty(&output->head_list)) {
		weston_log("Error: cannot enable output '%s' without heads.\n",
			   output->name);
		return -1;
	}

	if (wl_list_empty(&output->mode_list) || !output->current_mode) {
		weston_log("Error: no video mode for output '%s'.\n",
			   output->name);
		return -1;
	}

	wl_list_for_each(head, &output->head_list, output_link) {
		assert(head->make);
		assert(head->model);
	}

	iterator = container_of(c->output_list.prev,
				struct weston_output, link);

	if (!wl_list_empty(&c->output_list))
		x = iterator->x + iterator->width;

	/* Make sure the scale is set up */
	assert(output->scale);

	/* Make sure we have a transform set */
	assert(output->transform != UINT32_MAX);

	output->x = x;
	output->y = y;
	output->dirty = 1;
	output->original_scale = output->scale;

	wl_signal_init(&output->frame_signal);
	wl_signal_init(&output->destroy_signal);

	weston_output_transform_scale_init(output, output->transform, output->scale);
	weston_output_init_zoom(output);

	weston_output_init_geometry(output, x, y);
	weston_output_damage(output);

	wl_list_init(&output->animation_list);
	wl_list_init(&output->feedback_list);

	/* Enable the output (set up the crtc or create a
	 * window representing the output, set up the
	 * renderer, etc)
	 */
	if (output->enable(output) < 0) {
		weston_log("Enabling output \"%s\" failed.\n", output->name);
		return -1;
	}

	weston_compositor_add_output(output->compositor, output);

	head_names = weston_output_create_heads_string(output);
	weston_log("Output '%s' enabled with head(s) %s\n",
		   output->name, head_names);
	free(head_names);

	return 0;
}

/** Converts a weston_output object to a pending output state, so it
 ** can be configured again or destroyed.
 *
 * \param output The weston_output object that needs to be disabled.
 *
 * Calls a backend specific function to disable an output, in case
 * such function exists.
 *
 * The backend specific disable function may choose to postpone the disabling
 * by returning a negative value, in which case this function returns early.
 * In that case the backend will guarantee the output will be disabled soon
 * by the backend calling this function again. One must not attempt to re-enable
 * the output until that happens.
 *
 * Otherwise, if the output is being used by the compositor, it is removed
 * from weston's output_list (see weston_compositor_remove_output())
 * and is returned to a state it was before weston_output_enable()
 * was ran (see weston_output_enable_undo()).
 *
 * See weston_output_init() for more information on the
 * state output is returned to.
 *
 * If the output has never been enabled yet, this function can still be
 * called to ensure that the output is actually turned off rather than left
 * in the state it was discovered in.
 *
 * \ingroup output
 */
WL_EXPORT void
weston_output_disable(struct weston_output *output)
{
	/* Should we rename this? */
	output->destroying = 1;

	/* Disable is called unconditionally also for not-enabled outputs,
	 * because at compositor start-up, if there is an output that is
	 * already on but the compositor wants to turn it off, we have to
	 * forward the turn-off to the backend so it knows to do it.
	 * The backend cannot initially turn off everything, because it
	 * would cause unnecessary mode-sets for all outputs the compositor
	 * wants to be on.
	 */
	if (output->disable(output) < 0)
		return;

	if (output->enabled)
		weston_compositor_remove_output(output);

	output->destroying = 0;
}

/** Forces a synchronous call to heads_changed hook
 *
 * \param compositor The compositor instance
 *
 * If there are new or changed heads, calls the heads_changed hook and
 * returns after the hook returns.
 *
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_flush_heads_changed(struct weston_compositor *compositor)
{
	if (compositor->heads_changed_source) {
		wl_event_source_remove(compositor->heads_changed_source);
		weston_compositor_call_heads_changed(compositor);
	}
}

/** Add destroy callback for an output
 *
 * \param output The output to watch.
 * \param listener The listener to add. The \c notify member must be set.
 *
 * The listener callback will be called when user destroys an output. This
 * may be delayed by a backend in some cases. The main purpose of the
 * listener is to allow hooking up custom data to the output. The custom data
 * can be fetched via weston_output_get_destroy_listener() followed by
 * container_of().
 *
 * The \c data argument to the notify callback is the weston_output being
 * destroyed.
 *
 * @note This is for the final destruction of an output, not when it gets
 * disabled. If you want to keep track of enabled outputs, this is not it.
 *
 * \ingroup ouput
 */
WL_EXPORT void
weston_output_add_destroy_listener(struct weston_output *output,
				   struct wl_listener *listener)
{
	wl_signal_add(&output->user_destroy_signal, listener);
}

/** Look up destroy listener for an output
 *
 * \param output The output to query.
 * \param notify The notify function used used for the added destroy listener.
 * \return The listener, or NULL if not found.
 *
 * This looks up the previously added destroy listener struct based on the
 * notify function it has. The listener can be used to access user data
 * through \c container_of().
 *
 * \sa wl_signal_get() weston_output_add_destroy_listener()
 * \ingroup output
 */
WL_EXPORT struct wl_listener *
weston_output_get_destroy_listener(struct weston_output *output,
				   wl_notify_func_t notify)
{
	return wl_signal_get(&output->user_destroy_signal, notify);
}

/** Uninitialize an output
 *
 * Removes the output from the list of enabled outputs if necessary, but
 * does not call the backend's output disable function. The output will no
 * longer be in the list of pending outputs either.
 *
 * All fields of weston_output become uninitialized, i.e. should not be used
 * anymore. The caller can free the memory after this.
 *
 * \ingroup ouput
 * \internal
 */
WL_EXPORT void
weston_output_release(struct weston_output *output)
{
	struct weston_head *head, *tmp;

	output->destroying = 1;

	wl_signal_emit(&output->user_destroy_signal, output);

	if (output->idle_repaint_source)
		wl_event_source_remove(output->idle_repaint_source);

	if (output->enabled)
		weston_compositor_remove_output(output);

	pixman_region32_fini(&output->region);
	wl_list_remove(&output->link);

	wl_list_for_each_safe(head, tmp, &output->head_list, output_link)
		weston_head_detach(head);

	free(output->name);
}

/** Find an output by its given name
 *
 * \param compositor The compositor to search in.
 * \param name The output name to search for.
 * \return An existing output with the given name, or NULL if not found.
 *
 * \ingroup compositor
 */
WL_EXPORT struct weston_output *
weston_compositor_find_output_by_name(struct weston_compositor *compositor,
				      const char *name)
{
	struct weston_output *output;

	wl_list_for_each(output, &compositor->output_list, link)
		if (strcmp(output->name, name) == 0)
			return output;

	wl_list_for_each(output, &compositor->pending_output_list, link)
		if (strcmp(output->name, name) == 0)
			return output;

	return NULL;
}

/** Create a named output
 *
 * \param compositor The compositor.
 * \param name The name for the output.
 * \return A new \c weston_output, or NULL on failure.
 *
 * This creates a new weston_output that starts with no heads attached.
 *
 * An output must be configured and it must have at least one head before
 * it can be enabled.
 *
 * \ingroup compositor
 */
WL_EXPORT struct weston_output *
weston_compositor_create_output(struct weston_compositor *compositor,
				const char *name)
{
	assert(compositor->backend->create_output);

	if (weston_compositor_find_output_by_name(compositor, name)) {
		weston_log("Warning: attempted to create an output with a "
			   "duplicate name '%s'.\n", name);
		return NULL;
	}

	return compositor->backend->create_output(compositor, name);
}

/** Create an output for an unused head
 *
 * \param compositor The compositor.
 * \param head The head to attach to the output.
 * \return A new \c weston_output, or NULL on failure.
 *
 * This creates a new weston_output that starts with the given head attached.
 * The output inherits the name of the head. The head must not be already
 * attached to another output.
 *
 * An output must be configured before it can be enabled.
 *
 * \ingroup compositor
 */
WL_EXPORT struct weston_output *
weston_compositor_create_output_with_head(struct weston_compositor *compositor,
					  struct weston_head *head)
{
	struct weston_output *output;

	output = weston_compositor_create_output(compositor, head->name);
	if (!output)
		return NULL;

	if (weston_output_attach_head(output, head) < 0) {
		weston_output_destroy(output);
		return NULL;
	}

	return output;
}

/** Destroy an output
 *
 * \param output The output to destroy.
 *
 * The heads attached to the given output are detached and become unused again.
 *
 * It is not necessary to explicitly destroy all outputs at compositor exit.
 * weston_compositor_destroy() will automatically destroy any remaining
 * outputs.
 *
 * \ingroup ouput
 */
WL_EXPORT void
weston_output_destroy(struct weston_output *output)
{
	output->destroy(output);
}

/** When you need a head...
 *
 * This function is a hack, used until all code has been converted to become
 * multi-head aware.
 *
 * \param output The weston_output whose head to get.
 * \return The first head in the output's list.
 *
 * \ingroup ouput
 */
WL_EXPORT struct weston_head *
weston_output_get_first_head(struct weston_output *output)
{
	if (wl_list_empty(&output->head_list))
		return NULL;

	return container_of(output->head_list.next,
			    struct weston_head, output_link);
}

/** Allow/Disallow content-protection support for an output
 *
 * This function sets the allow_protection member for an output. Setting of
 * this field will allow the compositor to attempt content-protection for this
 * output, for a backend that supports the content-protection protocol.
 *
 * \param output The weston_output for whom the content-protection is to be
 * allowed.
 * \param allow_protection The bool value which is to be set.
 */
WL_EXPORT void
weston_output_allow_protection(struct weston_output *output,
			       bool allow_protection)
{
	output->allow_protection = allow_protection;
}

static void
xdg_output_unlist(struct wl_resource *resource)
{
	wl_list_remove(wl_resource_get_link(resource));
}

static void
xdg_output_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface xdg_output_interface = {
	xdg_output_destroy
};

static void
xdg_output_manager_destroy(struct wl_client *client,
                           struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
xdg_output_manager_get_xdg_output(struct wl_client *client,
				  struct wl_resource *manager,
				  uint32_t id,
				  struct wl_resource *output_resource)
{
	int version = wl_resource_get_version(manager);
	struct weston_head *head = wl_resource_get_user_data(output_resource);
	struct weston_output *output = head->output;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &zxdg_output_v1_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_list_insert(&head->xdg_output_resource_list,
		       wl_resource_get_link(resource));

	wl_resource_set_implementation(resource, &xdg_output_interface,
				       NULL, xdg_output_unlist);

	zxdg_output_v1_send_logical_position(resource, output->x, output->y);
	zxdg_output_v1_send_logical_size(resource,
					 output->width,
					 output->height);
	if (version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION)
		zxdg_output_v1_send_name(resource, head->name);

	zxdg_output_v1_send_done(resource);
}

static const struct zxdg_output_manager_v1_interface xdg_output_manager_interface = {
	xdg_output_manager_destroy,
	xdg_output_manager_get_xdg_output
};

static void
bind_xdg_output_manager(struct wl_client *client,
			void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &zxdg_output_manager_v1_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &xdg_output_manager_interface,
				       NULL, NULL);
}

static void
destroy_viewport(struct wl_resource *resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(resource);

	if (!surface)
		return;

	surface->viewport_resource = NULL;
	surface->pending.buffer_viewport.buffer.src_width =
		wl_fixed_from_int(-1);
	surface->pending.buffer_viewport.surface.width = -1;
	surface->pending.buffer_viewport.changed = 1;
}

static void
viewport_destroy(struct wl_client *client,
		 struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
viewport_set_source(struct wl_client *client,
		    struct wl_resource *resource,
		    wl_fixed_t src_x,
		    wl_fixed_t src_y,
		    wl_fixed_t src_width,
		    wl_fixed_t src_height)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(resource);

	if (!surface) {
		wl_resource_post_error(resource,
			WP_VIEWPORT_ERROR_NO_SURFACE,
			"wl_surface for this viewport is no longer exists");
		return;
	}

	assert(surface->viewport_resource == resource);
	assert(surface->resource);

	if (src_width == wl_fixed_from_int(-1) &&
	    src_height == wl_fixed_from_int(-1) &&
	    src_x == wl_fixed_from_int(-1) &&
	    src_y == wl_fixed_from_int(-1)) {
		/* unset source rect */
		surface->pending.buffer_viewport.buffer.src_width =
			wl_fixed_from_int(-1);
		surface->pending.buffer_viewport.changed = 1;
		return;
	}

	if (src_width <= 0 || src_height <= 0 || src_x < 0 || src_y < 0) {
		wl_resource_post_error(resource,
			WP_VIEWPORT_ERROR_BAD_VALUE,
			"wl_surface@%d viewport source "
			"w=%f <= 0, h=%f <= 0, x=%f < 0, or y=%f < 0",
			wl_resource_get_id(surface->resource),
			wl_fixed_to_double(src_width),
			wl_fixed_to_double(src_height),
			wl_fixed_to_double(src_x),
			wl_fixed_to_double(src_y));
		return;
	}

	surface->pending.buffer_viewport.buffer.src_x = src_x;
	surface->pending.buffer_viewport.buffer.src_y = src_y;
	surface->pending.buffer_viewport.buffer.src_width = src_width;
	surface->pending.buffer_viewport.buffer.src_height = src_height;
	surface->pending.buffer_viewport.changed = 1;
}

static void
viewport_set_destination(struct wl_client *client,
			 struct wl_resource *resource,
			 int32_t dst_width,
			 int32_t dst_height)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(resource);

	if (!surface) {
		wl_resource_post_error(resource,
			WP_VIEWPORT_ERROR_NO_SURFACE,
			"wl_surface for this viewport no longer exists");
		return;
	}

	assert(surface->viewport_resource == resource);

	if (dst_width == -1 && dst_height == -1) {
		/* unset destination size */
		surface->pending.buffer_viewport.surface.width = -1;
		surface->pending.buffer_viewport.changed = 1;
		return;
	}

	if (dst_width <= 0 || dst_height <= 0) {
		wl_resource_post_error(resource,
			WP_VIEWPORT_ERROR_BAD_VALUE,
			"destination size must be positive (%dx%d)",
			dst_width, dst_height);
		return;
	}

	surface->pending.buffer_viewport.surface.width = dst_width;
	surface->pending.buffer_viewport.surface.height = dst_height;
	surface->pending.buffer_viewport.changed = 1;
}

static const struct wp_viewport_interface viewport_interface = {
	viewport_destroy,
	viewport_set_source,
	viewport_set_destination
};

static void
viewporter_destroy(struct wl_client *client,
		   struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
viewporter_get_viewport(struct wl_client *client,
			struct wl_resource *viewporter,
			uint32_t id,
			struct wl_resource *surface_resource)
{
	int version = wl_resource_get_version(viewporter);
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct wl_resource *resource;

	if (surface->viewport_resource) {
		wl_resource_post_error(viewporter,
			WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
			"a viewport for that surface already exists");
		return;
	}

	resource = wl_resource_create(client, &wp_viewport_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &viewport_interface,
				       surface, destroy_viewport);

	surface->viewport_resource = resource;
}

static const struct wp_viewporter_interface viewporter_interface = {
	viewporter_destroy,
	viewporter_get_viewport
};

static void
bind_viewporter(struct wl_client *client,
		void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wp_viewporter_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &viewporter_interface,
				       NULL, NULL);
}

static void
destroy_presentation_feedback(struct wl_resource *feedback_resource)
{
	struct weston_presentation_feedback *feedback;

	feedback = wl_resource_get_user_data(feedback_resource);

	wl_list_remove(&feedback->link);
	free(feedback);
}

static void
presentation_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
presentation_feedback(struct wl_client *client,
		      struct wl_resource *presentation_resource,
		      struct wl_resource *surface_resource,
		      uint32_t callback)
{
	struct weston_surface *surface;
	struct weston_presentation_feedback *feedback;

	surface = wl_resource_get_user_data(surface_resource);

	feedback = zalloc(sizeof *feedback);
	if (feedback == NULL)
		goto err_calloc;

	feedback->resource = wl_resource_create(client,
					&wp_presentation_feedback_interface,
					1, callback);
	if (!feedback->resource)
		goto err_create;

	wl_resource_set_implementation(feedback->resource, NULL, feedback,
				       destroy_presentation_feedback);

	wl_list_insert(&surface->pending.feedback_list, &feedback->link);

	return;

err_create:
	free(feedback);

err_calloc:
	wl_client_post_no_memory(client);
}

static const struct wp_presentation_interface presentation_implementation = {
	presentation_destroy,
	presentation_feedback
};

static void
bind_presentation(struct wl_client *client,
		  void *data, uint32_t version, uint32_t id)
{
	struct weston_compositor *compositor = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wp_presentation_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &presentation_implementation,
				       compositor, NULL);
	wp_presentation_send_clock_id(resource, compositor->presentation_clock);
}

static void
compositor_bind(struct wl_client *client,
		void *data, uint32_t version, uint32_t id)
{
	struct weston_compositor *compositor = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wl_compositor_interface,
				      version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &compositor_interface,
				       compositor, NULL);
}

static const char *
output_repaint_status_text(struct weston_output *output)
{
	switch (output->repaint_status) {
	case REPAINT_NOT_SCHEDULED:
		return "no repaint";
	case REPAINT_BEGIN_FROM_IDLE:
		return "start_repaint_loop scheduled";
	case REPAINT_SCHEDULED:
		return "repaint scheduled";
	case REPAINT_AWAITING_COMPLETION:
		return "awaiting completion";
	}

	assert(!"output_repaint_status_text missing enum");
	return NULL;
}

static void
debug_scene_view_print_buffer(FILE *fp, struct weston_view *view)
{
	struct weston_buffer *buffer = view->surface->buffer_ref.buffer;
	struct wl_shm_buffer *shm;
	struct linux_dmabuf_buffer *dmabuf;
	const struct pixel_format_info *pixel_info = NULL;

	if (!buffer) {
		fprintf(fp, "\t\t[buffer not available]\n");
		return;
	}

	shm = wl_shm_buffer_get(buffer->resource);
	if (shm) {
		uint32_t _format = wl_shm_buffer_get_format(shm);
		pixel_info = pixel_format_get_info_shm(_format);
		fprintf(fp, "\t\tSHM buffer\n");
		fprintf(fp, "\t\t\tformat: 0x%lx %s\n",
			(unsigned long) _format,
			pixel_info ? pixel_info->drm_format_name : "UNKNOWN");
		return;
	}

	dmabuf = linux_dmabuf_buffer_get(buffer->resource);
	if (dmabuf) {
		pixel_info = pixel_format_get_info(dmabuf->attributes.format);
		fprintf(fp, "\t\tdmabuf buffer\n");
		fprintf(fp, "\t\t\tformat: 0x%lx %s\n",
			(unsigned long) dmabuf->attributes.format,
			pixel_info ? pixel_info->drm_format_name : "UNKNOWN");
		fprintf(fp, "\t\t\tmodifier: 0x%llx\n",
			(unsigned long long) dmabuf->attributes.modifier[0]);
		return;
	}

	fprintf(fp, "\t\tEGL buffer\n");
}

static void
debug_scene_view_print(FILE *fp, struct weston_view *view, int view_idx)
{
	struct weston_compositor *ec = view->surface->compositor;
	struct weston_output *output;
	char desc[512];
	pixman_box32_t *box;
	uint32_t surface_id = 0;
	pid_t pid = 0;

	if (view->surface->resource) {
		struct wl_resource *resource = view->surface->resource;
		wl_client_get_credentials(wl_resource_get_client(resource),
				  	  &pid, NULL, NULL);
		surface_id = wl_resource_get_id(view->surface->resource);
	}

	if (!view->surface->get_label ||
	    view->surface->get_label(view->surface, desc, sizeof(desc)) < 0) {
		strcpy(desc, "[no description available]");
	}
	fprintf(fp, "\tView %d (role %s, PID %d, surface ID %u, %s, %p):\n",
		view_idx, view->surface->role_name, pid, surface_id,
		desc, view);

	box = pixman_region32_extents(&view->transform.boundingbox);
	fprintf(fp, "\t\tposition: (%d, %d) -> (%d, %d)\n",
		box->x1, box->y1, box->x2, box->y2);
	box = pixman_region32_extents(&view->transform.opaque);

	if (weston_view_is_opaque(view, &view->transform.boundingbox)) {
		fprintf(fp, "\t\t[fully opaque]\n");
	} else if (!pixman_region32_not_empty(&view->transform.opaque)) {
		fprintf(fp, "\t\t[not opaque]\n");
	} else {
		fprintf(fp, "\t\t[opaque: (%d, %d) -> (%d, %d)]\n",
			box->x1, box->y1, box->x2, box->y2);
	}

	if (view->alpha < 1.0)
		fprintf(fp, "\t\talpha: %f\n", view->alpha);

	if (view->output_mask != 0) {
		bool first_output = true;
		fprintf(fp, "\t\toutputs: ");
		wl_list_for_each(output, &ec->output_list, link) {
			if (!(view->output_mask & (1 << output->id)))
				continue;
			fprintf(fp, "%s%d (%s)%s",
				(first_output) ? "" : ", ",
				output->id, output->name,
				(view->output == output) ? " (primary)" : "");
			first_output = false;
		}
	} else {
		fprintf(fp, "\t\t[no outputs]");
	}

	fprintf(fp, "\n");

	debug_scene_view_print_buffer(fp, view);
}

static void
debug_scene_view_print_tree(struct weston_view *view,
			    FILE *fp, int *view_idx)
{
	struct weston_subsurface *sub;
	struct weston_view *ev;

	/*
	 * print the view first, then we recursively go on printing
	 * sub-surfaces. We bail out once no more sub-surfaces are available.
	 */
	debug_scene_view_print(fp, view, *view_idx);

	/* no more sub-surfaces */
	if (wl_list_empty(&view->surface->subsurface_list))
		return;

	wl_list_for_each(sub, &view->surface->subsurface_list, parent_link) {
		wl_list_for_each(ev, &sub->surface->views, surface_link) {
			/* only print the child views of the current view */
			if (ev->parent_view != view)
				continue;

			(*view_idx)++;
			debug_scene_view_print_tree(ev, fp, view_idx);
		}
	}
}

/**
 * Output information on how libweston is currently composing the scene
 * graph.
 *
 * \ingroup compositor
 */
WL_EXPORT char *
weston_compositor_print_scene_graph(struct weston_compositor *ec)
{
	struct weston_output *output;
	struct weston_layer *layer;
	struct timespec now;
	int layer_idx = 0;
	FILE *fp;
	char *ret;
	size_t len;
	int err;

	fp = open_memstream(&ret, &len);
	assert(fp);

	weston_compositor_read_presentation_clock(ec, &now);
	fprintf(fp, "Weston scene graph at %ld.%09ld:\n\n",
		now.tv_sec, now.tv_nsec);

	wl_list_for_each(output, &ec->output_list, link) {
		struct weston_head *head;
		int head_idx = 0;

		fprintf(fp, "Output %d (%s):\n", output->id, output->name);
		assert(output->enabled);

		fprintf(fp, "\tposition: (%d, %d) -> (%d, %d)\n",
			output->x, output->y,
			output->x + output->width,
			output->y + output->height);
		fprintf(fp, "\tmode: %dx%d@%.3fHz\n",
			output->current_mode->width,
			output->current_mode->height,
			output->current_mode->refresh / 1000.0);
		fprintf(fp, "\tscale: %d\n", output->scale);

		fprintf(fp, "\trepaint status: %s\n",
			output_repaint_status_text(output));
		if (output->repaint_status == REPAINT_SCHEDULED)
			fprintf(fp, "\tnext repaint: %ld.%09ld\n",
				output->next_repaint.tv_sec,
				output->next_repaint.tv_nsec);

		wl_list_for_each(head, &output->head_list, output_link) {
			fprintf(fp, "\tHead %d (%s): %sconnected\n",
				head_idx++, head->name,
				(head->connected) ? "" : "not ");
		}
	}

	fprintf(fp, "\n");

	wl_list_for_each(layer, &ec->layer_list, link) {
		struct weston_view *view;
		int view_idx = 0;

		fprintf(fp, "Layer %d (pos 0x%lx):\n", layer_idx++,
			(unsigned long) layer->position);

		if (!weston_layer_mask_is_infinite(layer)) {
			fprintf(fp, "\t[mask: (%d, %d) -> (%d,%d)]\n\n",
				layer->mask.x1, layer->mask.y1,
				layer->mask.x2, layer->mask.y2);
		}

		wl_list_for_each(view, &layer->view_list.link, layer_link.link) {
			debug_scene_view_print_tree(view, fp, &view_idx);
			view_idx++;
		}

		if (wl_list_empty(&layer->view_list.link))
			fprintf(fp, "\t[no views]\n");

		fprintf(fp, "\n");
	}

	err = fclose(fp);
	assert(err == 0);

	return ret;
}

/**
 * Called when the 'scene-graph' debug scope is bound by a client. This
 * one-shot weston-debug scope prints the current scene graph when bound,
 * and then terminates the stream.
 */
static void
debug_scene_graph_cb(struct weston_log_subscription *sub, void *data)
{
	struct weston_compositor *ec = data;
	char *str = weston_compositor_print_scene_graph(ec);

	weston_log_subscription_printf(sub, "%s", str);
	free(str);
	weston_log_subscription_complete(sub);
}

/** Create the compositor.
 *
 * This functions creates and initializes a compositor instance.
 *
 * \param display The Wayland display to be used.
 * \param user_data A pointer to an object that can later be retrieved
 * \param log_ctx A pointer to weston_debug_compositor
 * using the \ref weston_compositor_get_user_data function.
 * \return The compositor instance on success or NULL on failure.
 *
 * \ingroup compositor
 */
WL_EXPORT struct weston_compositor *
weston_compositor_create(struct wl_display *display,
			 struct weston_log_context *log_ctx,
			 void *user_data)
{
	struct weston_compositor *ec;
	struct wl_event_loop *loop;

	if (!log_ctx)
		return NULL;

	ec = zalloc(sizeof *ec);
	if (!ec)
		return NULL;

	ec->weston_log_ctx = log_ctx;
	ec->wl_display = display;
	ec->user_data = user_data;
	wl_signal_init(&ec->destroy_signal);
	wl_signal_init(&ec->create_surface_signal);
	wl_signal_init(&ec->activate_signal);
	wl_signal_init(&ec->transform_signal);
	wl_signal_init(&ec->kill_signal);
	wl_signal_init(&ec->idle_signal);
	wl_signal_init(&ec->wake_signal);
	wl_signal_init(&ec->show_input_panel_signal);
	wl_signal_init(&ec->hide_input_panel_signal);
	wl_signal_init(&ec->update_input_panel_signal);
	wl_signal_init(&ec->seat_created_signal);
	wl_signal_init(&ec->output_created_signal);
	wl_signal_init(&ec->output_destroyed_signal);
	wl_signal_init(&ec->output_moved_signal);
	wl_signal_init(&ec->output_resized_signal);
	wl_signal_init(&ec->heads_changed_signal);
	wl_signal_init(&ec->output_heads_changed_signal);
	wl_signal_init(&ec->session_signal);
	ec->session_active = true;

	ec->output_id_pool = 0;
	ec->repaint_msec = DEFAULT_REPAINT_WINDOW;

	ec->activate_serial = 1;

	ec->touch_mode = WESTON_TOUCH_MODE_NORMAL;

	ec->content_protection = NULL;

	if (!wl_global_create(ec->wl_display, &wl_compositor_interface, 4,
			      ec, compositor_bind))
		goto fail;

	if (!wl_global_create(ec->wl_display, &wl_subcompositor_interface, 1,
			      ec, bind_subcompositor))
		goto fail;

	if (!wl_global_create(ec->wl_display, &wp_viewporter_interface, 1,
			      ec, bind_viewporter))
		goto fail;

	if (!wl_global_create(ec->wl_display, &zxdg_output_manager_v1_interface, 2,
			      ec, bind_xdg_output_manager))
		goto fail;

	if (!wl_global_create(ec->wl_display, &wp_presentation_interface, 1,
			      ec, bind_presentation))
		goto fail;

	if (weston_input_init(ec) != 0)
		goto fail;

	wl_list_init(&ec->view_list);
	wl_list_init(&ec->plane_list);
	wl_list_init(&ec->layer_list);
	wl_list_init(&ec->seat_list);
	wl_list_init(&ec->pending_output_list);
	wl_list_init(&ec->output_list);
	wl_list_init(&ec->head_list);
	wl_list_init(&ec->key_binding_list);
	wl_list_init(&ec->modifier_binding_list);
	wl_list_init(&ec->button_binding_list);
	wl_list_init(&ec->touch_binding_list);
	wl_list_init(&ec->axis_binding_list);
	wl_list_init(&ec->debug_binding_list);

	wl_list_init(&ec->plugin_api_list);

	weston_plane_init(&ec->primary_plane, ec, 0, 0);
	weston_compositor_stack_plane(ec, &ec->primary_plane, NULL);

	wl_data_device_manager_init(ec->wl_display);

	wl_display_init_shm(ec->wl_display);

	loop = wl_display_get_event_loop(ec->wl_display);
	ec->idle_source = wl_event_loop_add_timer(loop, idle_handler, ec);
	ec->repaint_timer =
		wl_event_loop_add_timer(loop, output_repaint_timer_handler,
					ec);

	weston_layer_init(&ec->fade_layer, ec);
	weston_layer_init(&ec->cursor_layer, ec);

	weston_layer_set_position(&ec->fade_layer, WESTON_LAYER_POSITION_FADE);
	weston_layer_set_position(&ec->cursor_layer,
				  WESTON_LAYER_POSITION_CURSOR);

	ec->debug_scene =
		weston_compositor_add_log_scope(ec, "scene-graph",
						"Scene graph details\n",
						debug_scene_graph_cb, NULL,
						ec);

	ec->timeline =
		weston_compositor_add_log_scope(ec, "timeline",
						"Timeline event points\n",
						weston_timeline_create_subscription,
						weston_timeline_destroy_subscription,
						ec);
	return ec;

fail:
	free(ec);
	return NULL;
}

/** weston_compositor_shutdown
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_shutdown(struct weston_compositor *ec)
{
	struct weston_output *output, *next;

	wl_event_source_remove(ec->idle_source);

	/* Destroy all outputs associated with this compositor */
	wl_list_for_each_safe(output, next, &ec->output_list, link)
		output->destroy(output);

	/* Destroy all pending outputs associated with this compositor */
	wl_list_for_each_safe(output, next, &ec->pending_output_list, link)
		output->destroy(output);

	if (ec->renderer)
		ec->renderer->destroy(ec);

	weston_binding_list_destroy_all(&ec->key_binding_list);
	weston_binding_list_destroy_all(&ec->modifier_binding_list);
	weston_binding_list_destroy_all(&ec->button_binding_list);
	weston_binding_list_destroy_all(&ec->touch_binding_list);
	weston_binding_list_destroy_all(&ec->axis_binding_list);
	weston_binding_list_destroy_all(&ec->debug_binding_list);

	weston_plane_release(&ec->primary_plane);
}

/** weston_compositor_exit_with_code
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_exit_with_code(struct weston_compositor *compositor,
				 int exit_code)
{
	if (compositor->exit_code == EXIT_SUCCESS)
		compositor->exit_code = exit_code;

	weston_compositor_exit(compositor);
}

/** weston_compositor_set_default_pointer_grab
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_set_default_pointer_grab(struct weston_compositor *ec,
			const struct weston_pointer_grab_interface *interface)
{
	struct weston_seat *seat;

	ec->default_pointer_grab = interface;
	wl_list_for_each(seat, &ec->seat_list, link) {
		struct weston_pointer *pointer = weston_seat_get_pointer(seat);

		if (pointer)
			weston_pointer_set_default_grab(pointer, interface);
	}
}

/** weston_compositor_set_presentation_clock
 * \ingroup compositor
 */
WL_EXPORT int
weston_compositor_set_presentation_clock(struct weston_compositor *compositor,
					 clockid_t clk_id)
{
	struct timespec ts;

	if (clock_gettime(clk_id, &ts) < 0)
		return -1;

	compositor->presentation_clock = clk_id;

	return 0;
}

/** For choosing the software clock, when the display hardware or API
 * does not expose a compatible presentation timestamp.
 *
 * \ingroup compositor
 */
WL_EXPORT int
weston_compositor_set_presentation_clock_software(
					struct weston_compositor *compositor)
{
	/* In order of preference */
	static const clockid_t clocks[] = {
		CLOCK_MONOTONIC_RAW,	/* no jumps, no crawling */
		CLOCK_MONOTONIC_COARSE,	/* no jumps, may crawl, fast & coarse */
		CLOCK_MONOTONIC,	/* no jumps, may crawl */
		CLOCK_REALTIME_COARSE,	/* may jump and crawl, fast & coarse */
		CLOCK_REALTIME		/* may jump and crawl */
	};
	unsigned i;

	for (i = 0; i < ARRAY_LENGTH(clocks); i++)
		if (weston_compositor_set_presentation_clock(compositor,
							     clocks[i]) == 0)
			return 0;

	weston_log("Error: no suitable presentation clock available.\n");

	return -1;
}

/** Read the current time from the Presentation clock
 *
 * \param compositor
 * \param[out] ts The current time.
 *
 * \note Reading the current time in user space is always imprecise to some
 * degree.
 *
 * This function is never meant to fail. If reading the clock does fail,
 * an error message is logged and a zero time is returned. Callers are not
 * supposed to detect or react to failures.
 *
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_read_presentation_clock(
			const struct weston_compositor *compositor,
			struct timespec *ts)
{
	static bool warned;
	int ret;

	ret = clock_gettime(compositor->presentation_clock, ts);
	if (ret < 0) {
		ts->tv_sec = 0;
		ts->tv_nsec = 0;

		if (!warned)
			weston_log("Error: failure to read "
				   "the presentation clock %#x: '%s' (%d)\n",
				   compositor->presentation_clock,
				   strerror(errno), errno);
		warned = true;
	}
}

/** Import dmabuf buffer into current renderer
 *
 * \param compositor
 * \param buffer the dmabuf buffer to import
 * \return true on usable buffers, false otherwise
 *
 * This function tests that the linux_dmabuf_buffer is usable
 * for the current renderer. Returns false on unusable buffers. Usually
 * usability is tested by importing the dmabufs for composition.
 *
 * This hook is also used for detecting if the renderer supports
 * dmabufs at all. If the renderer hook is NULL, dmabufs are not
 * supported.
 *
 * \ingroup compositor
 */
WL_EXPORT bool
weston_compositor_import_dmabuf(struct weston_compositor *compositor,
				struct linux_dmabuf_buffer *buffer)
{
	struct weston_renderer *renderer;

	renderer = compositor->renderer;

	if (renderer->import_dmabuf == NULL)
		return false;

	return renderer->import_dmabuf(compositor, buffer);
}

WL_EXPORT bool
weston_compositor_dmabuf_can_scanout(struct weston_compositor *compositor,
		struct linux_dmabuf_buffer *buffer)
{
	struct weston_backend *backend = compositor->backend;

	if (backend->can_scanout_dmabuf == NULL)
		return false;

	return backend->can_scanout_dmabuf(compositor, buffer);
}

WL_EXPORT void
weston_version(int *major, int *minor, int *micro)
{
	*major = WESTON_VERSION_MAJOR;
	*minor = WESTON_VERSION_MINOR;
	*micro = WESTON_VERSION_MICRO;
}

/**
 * Attempts to find a module path from the module map specified in the
 * environment. If found, writes the full path into the path variable.
 *
 * The module map is a string in environment variable WESTON_MODULE_MAP, where
 * each entry is of the form "name=path" and entries are separated by
 * semicolons. Whitespace is significant.
 *
 * \param name The name to search for.
 * \param path Where the path is written to if found.
 * \param path_len Allocated bytes at \c path .
 * \returns The length of the string written to path on success, or 0 if the
 * module was not specified in the environment map or path_len was too small.
 */
WL_EXPORT size_t
weston_module_path_from_env(const char *name, char *path, size_t path_len)
{
	const char *mapping = getenv("WESTON_MODULE_MAP");
	const char *end;
	const int name_len = strlen(name);

	if (!mapping)
		return 0;

	end = mapping + strlen(mapping);
	while (mapping < end && *mapping) {
		const char *filename, *next;

		/* early out: impossibly short string */
		if (end - mapping < name_len + 1)
			return 0;

		filename = &mapping[name_len + 1];
		next = strchrnul(mapping, ';');

		if (strncmp(mapping, name, name_len) == 0 &&
		    mapping[name_len] == '=') {
			size_t file_len = next - filename; /* no trailing NUL */
			if (file_len >= path_len)
				return 0;
			strncpy(path, filename, file_len);
			path[file_len] = '\0';
			return file_len;
		}

		mapping = next + 1;
	}

	return 0;
}

WL_EXPORT void *
weston_load_module(const char *name, const char *entrypoint)
{
	char path[PATH_MAX];
	void *module, *init;
	size_t len;

	if (name == NULL)
		return NULL;

	if (name[0] != '/') {
		len = weston_module_path_from_env(name, path, sizeof path);
		if (len == 0)
			len = snprintf(path, sizeof path, "%s/%s",
				       LIBWESTON_MODULEDIR, name);
	} else {
		len = snprintf(path, sizeof path, "%s", name);
	}

	/* snprintf returns the length of the string it would've written,
	 * _excluding_ the NUL byte. So even being equal to the size of
	 * our buffer is an error here. */
	if (len >= sizeof path)
		return NULL;

	module = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
	if (module) {
		weston_log("Module '%s' already loaded\n", path);
	} else {
		weston_log("Loading module '%s'\n", path);
		module = dlopen(path, RTLD_NOW);
		if (!module) {
			weston_log("Failed to load module: %s\n", dlerror());
			return NULL;
		}
	}

	init = dlsym(module, entrypoint);
	if (!init) {
		weston_log("Failed to lookup init function: %s\n", dlerror());
		dlclose(module);
		return NULL;
	}

	return init;
}

/** Add a compositor destroy listener only once
 *
 * \param compositor The compositor whose destroy to watch for.
 * \param listener The listener struct to initialize.
 * \param destroy_handler The callback when compositor is destroyed.
 * \return True if listener is added, or false if there already is a listener
 * with the given \c destroy_handler.
 *
 * This function does nothing and returns false if the given callback function
 * is already present in the weston_compositor destroy callbacks list.
 * Otherwise, this function initializes the given listener with the given
 * callback pointer and adds it to the compositor's destroy callbacks list.
 *
 * This can be used to ensure that plugin initialization is done only once
 * in case the same plugin is loaded multiple times. If this function returns
 * false, the plugin should be already initialized successfully.
 *
 * All plugins should register a destroy listener for cleaning up. Note, that
 * the plugin destruction order is not guaranteed: plugins that depend on other
 * plugins must be able to be torn down in arbitrary order.
 *
 * \sa weston_compositor_destroy
 */
WL_EXPORT bool
weston_compositor_add_destroy_listener_once(struct weston_compositor *compositor,
					    struct wl_listener *listener,
					    wl_notify_func_t destroy_handler)
{
	if (wl_signal_get(&compositor->destroy_signal, destroy_handler))
		return false;

	listener->notify = destroy_handler;
	wl_signal_add(&compositor->destroy_signal, listener);
	return true;
}

/** Destroys the compositor.
 *
 * This function cleans up the compositor state and then destroys it.
 *
 * @param compositor The compositor to be destroyed.
 *
 * @ingroup compositor
 */
WL_EXPORT void
weston_compositor_destroy(struct weston_compositor *compositor)
{
	/* prevent further rendering while shutting down */
	compositor->state = WESTON_COMPOSITOR_OFFSCREEN;

	wl_signal_emit(&compositor->destroy_signal, compositor);

	weston_compositor_xkb_destroy(compositor);

	if (compositor->backend)
		compositor->backend->destroy(compositor);

	/* The backend is responsible for destroying the heads. */
	assert(wl_list_empty(&compositor->head_list));

	weston_plugin_api_destroy_list(compositor);

	if (compositor->heads_changed_source)
		wl_event_source_remove(compositor->heads_changed_source);

	weston_log_scope_destroy(compositor->debug_scene);
	compositor->debug_scene = NULL;

	weston_log_scope_destroy(compositor->timeline);
	compositor->timeline = NULL;

	free(compositor);
}

/** Instruct the compositor to exit.
 *
 * This functions does not directly destroy the compositor object, it merely
 * command it to start the tear down process. It is not guaranteed that the
 * tear down will happen immediately.
 *
 * \param compositor The compositor to tear down.
 *
 * \ingroup compositor
 */
WL_EXPORT void
weston_compositor_exit(struct weston_compositor *compositor)
{
	compositor->exit(compositor);
}

/** Return the user data stored in the compositor.
 *
 * This function returns the user data pointer set with user_data parameter
 * to the \ref weston_compositor_create function.
 *
 * \ingroup compositor
 */
WL_EXPORT void *
weston_compositor_get_user_data(struct weston_compositor *compositor)
{
	return compositor->user_data;
}

static const char * const backend_map[] = {
	[WESTON_BACKEND_DRM] =		"drm-backend.so",
	[WESTON_BACKEND_FBDEV] =	"fbdev-backend.so",
	[WESTON_BACKEND_HEADLESS] =	"headless-backend.so",
	[WESTON_BACKEND_RDP] =		"rdp-backend.so",
	[WESTON_BACKEND_VNC] =		"vnc-backend.so",
	[WESTON_BACKEND_WAYLAND] =	"wayland-backend.so",
	[WESTON_BACKEND_X11] =		"x11-backend.so",
};

/** Load a backend into a weston_compositor
 *
 * A backend must be loaded to make a weston_compositor work. A backend
 * provides input and output capabilities, and determines the renderer to use.
 *
 * \param compositor A compositor that has not had a backend loaded yet.
 * \param backend Name of the backend file.
 * \param config_base A pointer to a backend-specific configuration
 * structure's 'base' member.
 *
 * \return 0 on success, or -1 on error.
 *
 * \ingroup compositor
 */
WL_EXPORT int
weston_compositor_load_backend(struct weston_compositor *compositor,
			       enum weston_compositor_backend backend,
			       struct weston_backend_config *config_base)
{
	int (*backend_init)(struct weston_compositor *c,
			    struct weston_backend_config *config_base);

	if (compositor->backend) {
		weston_log("Error: attempt to load a backend when one is already loaded\n");
		return -1;
	}

	if (backend >= ARRAY_LENGTH(backend_map))
		return -1;

	backend_init = weston_load_module(backend_map[backend], "weston_backend_init");
	if (!backend_init)
		return -1;

	if (backend_init(compositor, config_base) < 0) {
		compositor->backend = NULL;
		return -1;
	}

	return 0;
}

/** weston_compositor_load_xwayland
 * \ingroup compositor
 */
WL_EXPORT int
weston_compositor_load_xwayland(struct weston_compositor *compositor)
{
	int (*module_init)(struct weston_compositor *ec);

	module_init = weston_load_module("xwayland.so", "weston_module_init");
	if (!module_init)
		return -1;
	if (module_init(compositor) < 0)
		return -1;
	return 0;
}

/** Resolve an internal compositor error by disconnecting the client.
 *
 * This function is used in cases when the wl_buffer turns out
 * unusable and there is no fallback path.
 *
 * It is possible the fault is caused by a compositor bug, the underlying
 * graphics stack bug or normal behaviour, or perhaps a client mistake.
 * In any case, the options are to either composite garbage or nothing,
 * or disconnect the client. This is a helper function for the latter.
 *
 * The error is sent as an INVALID_OBJECT error on the client's wl_display.
 *
 * \param buffer The weston buffer that is unusable.
 * \param msg A custom error message attached to the protocol error.
 */
WL_EXPORT void
weston_buffer_send_server_error(struct weston_buffer *buffer,
				      const char *msg)
{
	struct wl_client *client;
	struct wl_resource *display_resource;
	uint32_t id;

	assert(buffer->resource);
	id = wl_resource_get_id(buffer->resource);
	client = wl_resource_get_client(buffer->resource);
	display_resource = wl_client_get_object(client, 1);

	assert(display_resource);
	wl_resource_post_error(display_resource,
			       WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "server error with "
			       "wl_buffer@%u: %s", id, msg);
}

WL_EXPORT void
weston_output_disable_planes_incr(struct weston_output *output)
{
	output->disable_planes++;
	/*
	 * If disable_planes changes from 0 to non-zero, it means some type of
	 * recording of content has started, and therefore protection level of
	 * the protected surfaces must be updated to avoid the recording of
	 * the protected content.
	 */
	if (output->disable_planes == 1)
		weston_schedule_surface_protection_update(output->compositor);
}

WL_EXPORT void
weston_output_disable_planes_decr(struct weston_output *output)
{
	output->disable_planes--;
	/*
	 * If disable_planes changes from non-zero to 0, it means no content
	 * recording is going on any more, and the protected and surfaces can be
	 * shown without any apprehensions about content being recorded.
	 */
	if (output->disable_planes == 0)
		weston_schedule_surface_protection_update(output->compositor);

}
