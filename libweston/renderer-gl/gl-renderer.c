/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2015,2019,2021 Collabora, Ltd.
 * Copyright © 2016 NVIDIA Corporation
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

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>
#include <assert.h>
#include <linux/input.h>
#include <unistd.h>

#include "linux-sync-file.h"
#include "timeline.h"

#include "color.h"
#include "gl-renderer.h"
#include "gl-renderer-internal.h"
#include "vertex-clipping.h"
#include "linux-dmabuf.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "linux-explicit-synchronization.h"
#include "pixel-formats.h"

#include "shared/fd-util.h"
#include "shared/helpers.h"
#include "shared/platform.h"
#include "shared/timespec-util.h"
#include "shared/weston-drm-fourcc.h"
#include "shared/weston-egl-ext.h"

#define BUFFER_DAMAGE_COUNT 2

enum gl_border_status {
	BORDER_STATUS_CLEAN = 0,
	BORDER_TOP_DIRTY = 1 << GL_RENDERER_BORDER_TOP,
	BORDER_LEFT_DIRTY = 1 << GL_RENDERER_BORDER_LEFT,
	BORDER_RIGHT_DIRTY = 1 << GL_RENDERER_BORDER_RIGHT,
	BORDER_BOTTOM_DIRTY = 1 << GL_RENDERER_BORDER_BOTTOM,
	BORDER_ALL_DIRTY = 0xf,
	BORDER_SIZE_CHANGED = 0x10
};

struct gl_border_image {
	GLuint tex;
	int32_t width, height;
	int32_t tex_width;
	void *data;
};

struct gl_fbo_texture {
	GLuint fbo;
	GLuint tex;
	int32_t width;
	int32_t height;
};

struct gl_output_state {
	EGLSurface egl_surface;
	pixman_region32_t buffer_damage[BUFFER_DAMAGE_COUNT];
	int buffer_damage_index;
	enum gl_border_status border_damage[BUFFER_DAMAGE_COUNT];
	struct gl_border_image borders[4];
	enum gl_border_status border_status;
	bool swap_behavior_is_preserved;

	struct weston_matrix output_matrix;

	EGLSyncKHR begin_render_sync, end_render_sync;

	/* struct timeline_render_point::link */
	struct wl_list timeline_render_point_list;

	struct gl_fbo_texture shadow;
};

enum buffer_type {
	BUFFER_TYPE_NULL,
	BUFFER_TYPE_SOLID, /* internal solid color surfaces without a buffer */
	BUFFER_TYPE_SHM,
	BUFFER_TYPE_EGL
};

struct gl_renderer;

struct egl_image {
	struct gl_renderer *renderer;
	EGLImageKHR image;
	int refcount;
};

enum import_type {
	IMPORT_TYPE_INVALID,
	IMPORT_TYPE_DIRECT,
	IMPORT_TYPE_GL_CONVERSION
};

struct dmabuf_image {
	struct linux_dmabuf_buffer *dmabuf;
	int num_images;
	struct egl_image *images[3];
	struct wl_list link;

	enum import_type import_type;
	enum gl_shader_texture_variant shader_variant;
};

struct dmabuf_format {
	uint32_t format;
	struct wl_list link;

	uint64_t *modifiers;
	unsigned *external_only;
	int num_modifiers;
};

struct yuv_plane_descriptor {
	int width_divisor;
	int height_divisor;
	uint32_t format;
	int plane_index;
};

enum texture_type {
	TEXTURE_Y_XUXV_WL,
	TEXTURE_Y_UV_WL,
	TEXTURE_Y_U_V_WL,
	TEXTURE_XYUV_WL
};

struct yuv_format_descriptor {
	uint32_t format;
	int input_planes;
	int output_planes;
	enum texture_type texture_type;
	struct yuv_plane_descriptor plane[4];
};

struct gl_surface_state {
	GLfloat color[4];

	GLuint textures[3];
	int num_textures;
	bool needs_full_upload;
	pixman_region32_t texture_damage;

	/* These are only used by SHM surfaces to detect when we need
	 * to do a full upload to specify a new internal texture
	 * format */
	GLenum gl_format[3];
	GLenum gl_pixel_type;

	struct egl_image* images[3];
	int num_images;
	enum gl_shader_texture_variant shader_variant;

	struct weston_buffer_reference buffer_ref;
	struct weston_buffer_release_reference buffer_release_ref;
	enum buffer_type buffer_type;
	int pitch; /* in pixels */
	int height; /* in pixels */
	bool y_inverted;
	bool direct_display;

	/* Extension needed for SHM YUV texture */
	int offset[3]; /* offset per plane */
	int hsub[3];  /* horizontal subsampling per plane */
	int vsub[3];  /* vertical subsampling per plane */

	struct weston_surface *surface;

	/* Whether this surface was used in the current output repaint.
	   Used only in the context of a gl_renderer_repaint_output call. */
	bool used_in_output_repaint;

	struct wl_listener surface_destroy_listener;
	struct wl_listener renderer_destroy_listener;
};

enum timeline_render_point_type {
	TIMELINE_RENDER_POINT_TYPE_BEGIN,
	TIMELINE_RENDER_POINT_TYPE_END
};

struct timeline_render_point {
	struct wl_list link; /* gl_output_state::timeline_render_point_list */

	enum timeline_render_point_type type;
	int fd;
	struct weston_output *output;
	struct wl_event_source *event_source;
};

static uint32_t
gr_gl_version(uint16_t major, uint16_t minor)
{
	return ((uint32_t)major << 16) | minor;
}

static int
gr_gl_version_major(uint32_t ver)
{
	return ver >> 16;
}

static int
gr_gl_version_minor(uint32_t ver)
{
	return ver & 0xffff;
}

static inline const char *
dump_format(uint32_t format, char out[4])
{
#if BYTE_ORDER == BIG_ENDIAN
	format = __builtin_bswap32(format);
#endif
	memcpy(out, &format, 4);
	return out;
}

static inline struct gl_output_state *
get_output_state(struct weston_output *output)
{
	return (struct gl_output_state *)output->renderer_state;
}

static int
gl_renderer_create_surface(struct weston_surface *surface);

static inline struct gl_surface_state *
get_surface_state(struct weston_surface *surface)
{
	if (!surface->renderer_state)
		gl_renderer_create_surface(surface);

	return (struct gl_surface_state *)surface->renderer_state;
}

static bool
shadow_exists(const struct gl_output_state *go)
{
	return go->shadow.fbo != 0;
}

static void
timeline_render_point_destroy(struct timeline_render_point *trp)
{
	wl_list_remove(&trp->link);
	wl_event_source_remove(trp->event_source);
	close(trp->fd);
	free(trp);
}

static int
timeline_render_point_handler(int fd, uint32_t mask, void *data)
{
	struct timeline_render_point *trp = data;
	const char *tp_name = trp->type == TIMELINE_RENDER_POINT_TYPE_BEGIN ?
			      "renderer_gpu_begin" : "renderer_gpu_end";

	if (mask & WL_EVENT_READABLE) {
		struct timespec tspec = { 0 };

		if (weston_linux_sync_file_read_timestamp(trp->fd,
							  &tspec) == 0) {
			TL_POINT(trp->output->compositor, tp_name, TLP_GPU(&tspec),
				 TLP_OUTPUT(trp->output), TLP_END);
		}
	}

	timeline_render_point_destroy(trp);

	return 0;
}

static EGLSyncKHR
create_render_sync(struct gl_renderer *gr)
{
	static const EGLint attribs[] = { EGL_NONE };

	if (!gr->has_native_fence_sync)
		return EGL_NO_SYNC_KHR;

	return gr->create_sync(gr->egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID,
			       attribs);
}

static void
timeline_submit_render_sync(struct gl_renderer *gr,
			    struct weston_output *output,
			    EGLSyncKHR sync,
			    enum timeline_render_point_type type)
{
	struct gl_output_state *go;
	struct wl_event_loop *loop;
	int fd;
	struct timeline_render_point *trp;

	if (!weston_log_scope_is_enabled(gr->compositor->timeline) ||
	    !gr->has_native_fence_sync ||
	    sync == EGL_NO_SYNC_KHR)
		return;

	go = get_output_state(output);
	loop = wl_display_get_event_loop(gr->compositor->wl_display);

	fd = gr->dup_native_fence_fd(gr->egl_display, sync);
	if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID)
		return;

	trp = zalloc(sizeof *trp);
	if (trp == NULL) {
		close(fd);
		return;
	}

	trp->type = type;
	trp->fd = fd;
	trp->output = output;
	trp->event_source = wl_event_loop_add_fd(loop, fd,
						 WL_EVENT_READABLE,
						 timeline_render_point_handler,
						 trp);

	wl_list_insert(&go->timeline_render_point_list, &trp->link);
}

static struct egl_image*
egl_image_create(struct gl_renderer *gr, EGLenum target,
		 EGLClientBuffer buffer, const EGLint *attribs)
{
	struct egl_image *img;

	img = zalloc(sizeof *img);
	if (!img)
		return NULL;

	img->renderer = gr;
	img->refcount = 1;
	img->image = gr->create_image(gr->egl_display, EGL_NO_CONTEXT,
				      target, buffer, attribs);

	if (img->image == EGL_NO_IMAGE_KHR) {
		free(img);
		return NULL;
	}

	return img;
}

static struct egl_image*
egl_image_ref(struct egl_image *image)
{
	image->refcount++;

	return image;
}

static int
egl_image_unref(struct egl_image *image)
{
	struct gl_renderer *gr = image->renderer;

	assert(image->refcount > 0);

	image->refcount--;
	if (image->refcount > 0)
		return image->refcount;

	gr->destroy_image(gr->egl_display, image->image);
	free(image);

	return 0;
}

static struct dmabuf_image*
dmabuf_image_create(void)
{
	struct dmabuf_image *img;

	img = zalloc(sizeof *img);
	if (!img)
		return NULL;

	wl_list_init(&img->link);

	return img;
}

static void
dmabuf_image_destroy(struct dmabuf_image *image)
{
	int i;

	for (i = 0; i < image->num_images; ++i)
		egl_image_unref(image->images[i]);

	if (image->dmabuf)
		linux_dmabuf_buffer_set_user_data(image->dmabuf, NULL, NULL);

	wl_list_remove(&image->link);
	free(image);
}

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) > (b)) ? (b) : (a))

/*
 * Compute the boundary vertices of the intersection of the global coordinate
 * aligned rectangle 'rect', and an arbitrary quadrilateral produced from
 * 'surf_rect' when transformed from surface coordinates into global coordinates.
 * The vertices are written to 'ex' and 'ey', and the return value is the
 * number of vertices. Vertices are produced in clockwise winding order.
 * Guarantees to produce either zero vertices, or 3-8 vertices with non-zero
 * polygon area.
 */
static int
calculate_edges(struct weston_view *ev, pixman_box32_t *rect,
		pixman_box32_t *surf_rect, GLfloat *ex, GLfloat *ey)
{

	struct clip_context ctx;
	int i, n;
	GLfloat min_x, max_x, min_y, max_y;
	struct polygon8 surf = {
		{ surf_rect->x1, surf_rect->x2, surf_rect->x2, surf_rect->x1 },
		{ surf_rect->y1, surf_rect->y1, surf_rect->y2, surf_rect->y2 },
		4
	};

	ctx.clip.x1 = rect->x1;
	ctx.clip.y1 = rect->y1;
	ctx.clip.x2 = rect->x2;
	ctx.clip.y2 = rect->y2;

	/* transform surface to screen space: */
	for (i = 0; i < surf.n; i++)
		weston_view_to_global_float(ev, surf.x[i], surf.y[i],
					    &surf.x[i], &surf.y[i]);

	/* find bounding box: */
	min_x = max_x = surf.x[0];
	min_y = max_y = surf.y[0];

	for (i = 1; i < surf.n; i++) {
		min_x = min(min_x, surf.x[i]);
		max_x = max(max_x, surf.x[i]);
		min_y = min(min_y, surf.y[i]);
		max_y = max(max_y, surf.y[i]);
	}

	/* First, simple bounding box check to discard early transformed
	 * surface rects that do not intersect with the clip region:
	 */
	if ((min_x >= ctx.clip.x2) || (max_x <= ctx.clip.x1) ||
	    (min_y >= ctx.clip.y2) || (max_y <= ctx.clip.y1))
		return 0;

	/* Simple case, bounding box edges are parallel to surface edges,
	 * there will be only four edges.  We just need to clip the surface
	 * vertices to the clip rect bounds:
	 */
	if (!ev->transform.enabled)
		return clip_simple(&ctx, &surf, ex, ey);

	/* Transformed case: use a general polygon clipping algorithm to
	 * clip the surface rectangle with each side of 'rect'.
	 * The algorithm is Sutherland-Hodgman, as explained in
	 * http://www.codeguru.com/cpp/misc/misc/graphics/article.php/c8965/Polygon-Clipping.htm
	 * but without looking at any of that code.
	 */
	n = clip_transformed(&ctx, &surf, ex, ey);

	if (n < 3)
		return 0;

	return n;
}

static bool
merge_down(pixman_box32_t *a, pixman_box32_t *b, pixman_box32_t *merge)
{
	if (a->x1 == b->x1 && a->x2 == b->x2 && a->y1 == b->y2) {
		merge->x1 = a->x1;
		merge->x2 = a->x2;
		merge->y1 = b->y1;
		merge->y2 = a->y2;
		return true;
	}
	return false;
}

static int
compress_bands(pixman_box32_t *inrects, int nrects, pixman_box32_t **outrects)
{
	bool merged = false;
	pixman_box32_t *out, merge_rect;
	int i, j, nout;

	if (!nrects) {
		*outrects = NULL;
		return 0;
	}

	/* nrects is an upper bound - we're not too worried about
	 * allocating a little extra
	 */
	out = malloc(sizeof(pixman_box32_t) * nrects);
	out[0] = inrects[0];
	nout = 1;
	for (i = 1; i < nrects; i++) {
		for (j = 0; j < nout; j++) {
			merged = merge_down(&inrects[i], &out[j], &merge_rect);
			if (merged) {
				out[j] = merge_rect;
				break;
			}
		}
		if (!merged) {
			out[nout] = inrects[i];
			nout++;
		}
	}
	*outrects = out;
	return nout;
}

static int
texture_region(struct weston_view *ev,
	       pixman_region32_t *region,
	       pixman_region32_t *surf_region)
{
	struct gl_surface_state *gs = get_surface_state(ev->surface);
	struct weston_compositor *ec = ev->surface->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	GLfloat *v, inv_width, inv_height;
	unsigned int *vtxcnt, nvtx = 0;
	pixman_box32_t *rects, *surf_rects;
	pixman_box32_t *raw_rects;
	int i, j, k, nrects, nsurf, raw_nrects;
	bool used_band_compression;
	raw_rects = pixman_region32_rectangles(region, &raw_nrects);
	surf_rects = pixman_region32_rectangles(surf_region, &nsurf);

	if (raw_nrects < 4) {
		used_band_compression = false;
		nrects = raw_nrects;
		rects = raw_rects;
	} else {
		nrects = compress_bands(raw_rects, raw_nrects, &rects);
		used_band_compression = true;
	}
	/* worst case we can have 8 vertices per rect (ie. clipped into
	 * an octagon):
	 */
	v = wl_array_add(&gr->vertices, nrects * nsurf * 8 * 4 * sizeof *v);
	vtxcnt = wl_array_add(&gr->vtxcnt, nrects * nsurf * sizeof *vtxcnt);

	inv_width = 1.0 / gs->pitch;
        inv_height = 1.0 / gs->height;

	for (i = 0; i < nrects; i++) {
		pixman_box32_t *rect = &rects[i];
		for (j = 0; j < nsurf; j++) {
			pixman_box32_t *surf_rect = &surf_rects[j];
			GLfloat sx, sy, bx, by;
			GLfloat ex[8], ey[8];          /* edge points in screen space */
			int n;

			/* The transformed surface, after clipping to the clip region,
			 * can have as many as eight sides, emitted as a triangle-fan.
			 * The first vertex in the triangle fan can be chosen arbitrarily,
			 * since the area is guaranteed to be convex.
			 *
			 * If a corner of the transformed surface falls outside of the
			 * clip region, instead of emitting one vertex for the corner
			 * of the surface, up to two are emitted for two corresponding
			 * intersection point(s) between the surface and the clip region.
			 *
			 * To do this, we first calculate the (up to eight) points that
			 * form the intersection of the clip rect and the transformed
			 * surface.
			 */
			n = calculate_edges(ev, rect, surf_rect, ex, ey);
			if (n < 3)
				continue;

			/* emit edge points: */
			for (k = 0; k < n; k++) {
				weston_view_from_global_float(ev, ex[k], ey[k],
							      &sx, &sy);
				/* position: */
				*(v++) = ex[k];
				*(v++) = ey[k];
				/* texcoord: */
				weston_surface_to_buffer_float(ev->surface,
							       sx, sy,
							       &bx, &by);
				*(v++) = bx * inv_width;
				if (gs->y_inverted) {
					*(v++) = by * inv_height;
				} else {
					*(v++) = (gs->height - by) * inv_height;
				}
			}

			vtxcnt[nvtx++] = n;
		}
	}

	if (used_band_compression)
		free(rects);
	return nvtx;
}

/** Create a texture and a framebuffer object
 *
 * \param fbotex To be initialized.
 * \param width Texture width in pixels.
 * \param height Texture heigh in pixels.
 * \param internal_format See glTexImage2D.
 * \param format See glTexImage2D.
 * \param type See glTexImage2D.
 * \return True on success, false otherwise.
 */
static bool
gl_fbo_texture_init(struct gl_fbo_texture *fbotex,
			 int32_t width,
			 int32_t height,
			 GLint internal_format,
			 GLenum format,
			 GLenum type)
{
	int fb_status;
	GLuint shadow_fbo;
	GLuint shadow_tex;

	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &shadow_tex);
	glBindTexture(GL_TEXTURE_2D, shadow_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0,
		     format, type, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &shadow_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, shadow_tex, 0);

	fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
		glDeleteFramebuffers(1, &shadow_fbo);
		glDeleteTextures(1, &shadow_tex);
		return false;
	}

	fbotex->fbo = shadow_fbo;
	fbotex->tex = shadow_tex;
	fbotex->width = width;
	fbotex->height = height;

	return true;
}

static void
gl_fbo_texture_fini(struct gl_fbo_texture *fbotex)
{
	glDeleteFramebuffers(1, &fbotex->fbo);
	fbotex->fbo = 0;
	glDeleteTextures(1, &fbotex->tex);
	fbotex->tex = 0;
}

static void
gl_renderer_send_shader_error(struct weston_view *view)
{
	struct wl_resource *resource = view->surface->resource;

	if (!resource)
		return;

	wl_client_post_implementation_error(wl_resource_get_client(resource),
		"Weston GL-renderer shader failed for wl_surface@%u",
		wl_resource_get_id(resource));
}

static void
triangle_fan_debug(struct gl_renderer *gr,
		   const struct gl_shader_config *sconf,
		   int first, int count)
{
	int i;
	GLushort *buffer;
	GLushort *index;
	int nelems;
	static int color_idx = 0;
	struct gl_shader_config alt;
	const GLfloat *col;
	static const GLfloat color[][4] = {
			{ 1.0, 0.0, 0.0, 1.0 },
			{ 0.0, 1.0, 0.0, 1.0 },
			{ 0.0, 0.0, 1.0, 1.0 },
			{ 1.0, 1.0, 1.0, 1.0 },
	};

	col = color[color_idx++ % ARRAY_LENGTH(color)];
	alt = (struct gl_shader_config) {
		.req = {
			.variant = SHADER_VARIANT_SOLID,
		},
		.projection = sconf->projection,
		.view_alpha = 1.0f,
		.unicolor = { col[0], col[1], col[2], col[3] },
	};

	gl_renderer_use_program(gr, &alt);

	nelems = (count - 1 + count - 2) * 2;

	buffer = malloc(sizeof(GLushort) * nelems);
	index = buffer;

	for (i = 1; i < count; i++) {
		*index++ = first;
		*index++ = first + i;
	}

	for (i = 2; i < count; i++) {
		*index++ = first + i - 1;
		*index++ = first + i;
	}

	glDrawElements(GL_LINES, nelems, GL_UNSIGNED_SHORT, buffer);

	free(buffer);

	gl_renderer_use_program(gr, sconf);
}

static void
repaint_region(struct gl_renderer *gr,
	       struct weston_view *ev,
	       pixman_region32_t *region,
	       pixman_region32_t *surf_region,
	       const struct gl_shader_config *sconf)
{
	GLfloat *v;
	unsigned int *vtxcnt;
	int i, first, nfans;

	/* The final region to be painted is the intersection of
	 * 'region' and 'surf_region'. However, 'region' is in the global
	 * coordinates, and 'surf_region' is in the surface-local
	 * coordinates. texture_region() will iterate over all pairs of
	 * rectangles from both regions, compute the intersection
	 * polygon for each pair, and store it as a triangle fan if
	 * it has a non-zero area (at least 3 vertices, actually).
	 */
	nfans = texture_region(ev, region, surf_region);

	v = gr->vertices.data;
	vtxcnt = gr->vtxcnt.data;

	/* position: */
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[0]);
	glEnableVertexAttribArray(0);

	/* texcoord: */
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[2]);
	glEnableVertexAttribArray(1);

	if (!gl_renderer_use_program(gr, sconf)) {
		gl_renderer_send_shader_error(ev);
		/* continue drawing with the fallback shader */
	}

	for (i = 0, first = 0; i < nfans; i++) {
		glDrawArrays(GL_TRIANGLE_FAN, first, vtxcnt[i]);
		if (gr->fan_debug)
			triangle_fan_debug(gr, sconf, first, vtxcnt[i]);
		first += vtxcnt[i];
	}

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);

	gr->vertices.size = 0;
	gr->vtxcnt.size = 0;
}

static int
use_output(struct weston_output *output)
{
	static int errored;
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);
	EGLBoolean ret;

	ret = eglMakeCurrent(gr->egl_display, go->egl_surface,
			     go->egl_surface, gr->egl_context);

	if (ret == EGL_FALSE) {
		if (errored)
			return -1;
		errored = 1;
		weston_log("Failed to make EGL context current.\n");
		gl_renderer_print_egl_error_state();
		return -1;
	}

	return 0;
}

static int
ensure_surface_buffer_is_ready(struct gl_renderer *gr,
			       struct gl_surface_state *gs)
{
	EGLint attribs[] = {
		EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
		-1,
		EGL_NONE
	};
	struct weston_surface *surface = gs->surface;
	struct weston_buffer *buffer = gs->buffer_ref.buffer;
	EGLSyncKHR sync;
	EGLint wait_ret;
	EGLint destroy_ret;

	if (!buffer)
		return 0;

	if (surface->acquire_fence_fd < 0)
		return 0;

	/* We should only get a fence if we support EGLSyncKHR, since
	 * we don't advertise the explicit sync protocol otherwise. */
	assert(gr->has_native_fence_sync);
	/* We should only get a fence for non-SHM buffers, since surface
	 * commit would have failed otherwise. */
	assert(wl_shm_buffer_get(buffer->resource) == NULL);

	attribs[1] = dup(surface->acquire_fence_fd);
	if (attribs[1] == -1) {
		linux_explicit_synchronization_send_server_error(
			gs->surface->synchronization_resource,
			"Failed to dup acquire fence");
		return -1;
	}

	sync = gr->create_sync(gr->egl_display,
			       EGL_SYNC_NATIVE_FENCE_ANDROID,
			       attribs);
	if (sync == EGL_NO_SYNC_KHR) {
		linux_explicit_synchronization_send_server_error(
			gs->surface->synchronization_resource,
			"Failed to create EGLSyncKHR object");
		close(attribs[1]);
		return -1;
	}

	wait_ret = gr->wait_sync(gr->egl_display, sync, 0);
	if (wait_ret == EGL_FALSE) {
		linux_explicit_synchronization_send_server_error(
			gs->surface->synchronization_resource,
			"Failed to wait on EGLSyncKHR object");
		/* Continue to try to destroy the sync object. */
	}


	destroy_ret = gr->destroy_sync(gr->egl_display, sync);
	if (destroy_ret == EGL_FALSE) {
		linux_explicit_synchronization_send_server_error(
			gs->surface->synchronization_resource,
			"Failed to destroy on EGLSyncKHR object");
	}

	return (wait_ret == EGL_TRUE && destroy_ret == EGL_TRUE) ? 0 : -1;
}


 /* Checks if a view needs to be censored on an output
  * Checks for 2 types of censor requirements
  * - recording_censor: Censor protected view when a
  *   protected view is captured.
  * - unprotected_censor: Censor regions of protected views
  *   when displayed on an output which has lower protection capability.
  * If censoring is needed, smashes the GL shader config.
  */
static void
maybe_censor_override(struct gl_shader_config *sconf,
		      struct weston_output *output,
		      struct weston_view *ev)
{
	const struct gl_shader_config alt = {
		.req = {
			.variant = SHADER_VARIANT_SOLID,
		},
		.projection = sconf->projection,
		.view_alpha = sconf->view_alpha,
		.unicolor = { 0.40, 0.0, 0.0, 1.0 },
	};
	struct gl_surface_state *gs = get_surface_state(ev->surface);
	bool recording_censor =
		(output->disable_planes > 0) &&
		(ev->surface->desired_protection > WESTON_HDCP_DISABLE);

	bool unprotected_censor =
		(ev->surface->desired_protection > output->current_protection);

	if (gs->direct_display) {
		*sconf = alt;
		return;
	}

	/* When not in enforced mode, the client is notified of the protection */
	/* change, so content censoring is not required */
	if (ev->surface->protection_mode !=
	    WESTON_SURFACE_PROTECTION_MODE_ENFORCED)
		return;

	if (recording_censor || unprotected_censor)
		*sconf = alt;
}

static void
gl_shader_config_set_input_textures(struct gl_shader_config *sconf,
				    struct gl_surface_state *gs)
{
	int i;

	sconf->req.variant = gs->shader_variant;

	for (i = 0; i < 4; i++)
		sconf->unicolor[i] = gs->color[i];

	assert(gs->num_textures <= GL_SHADER_INPUT_TEX_MAX);
	for (i = 0; i < gs->num_textures; i++)
		sconf->input_tex[i] = gs->textures[i];
	for (; i < GL_SHADER_INPUT_TEX_MAX; i++)
		sconf->input_tex[i] = 0;
}

static bool
gl_shader_config_init_for_paint_node(struct gl_shader_config *sconf,
				     struct weston_paint_node *pnode,
				     GLint filter)
{
	struct gl_surface_state *gs = get_surface_state(pnode->surface);
	struct gl_output_state *go = get_output_state(pnode->output);

	if (!pnode->surf_xform_valid)
		return false;
	assert(pnode->surf_xform.transform == NULL);

	*sconf = (struct gl_shader_config) {
		.projection = go->output_matrix,
		.view_alpha = pnode->view->alpha,
		.input_tex_filter = filter,
	};

	gl_shader_config_set_input_textures(sconf, gs);

	return true;
}

static void
draw_paint_node(struct weston_paint_node *pnode,
		pixman_region32_t *damage /* in global coordinates */)
{
	struct gl_renderer *gr = get_renderer(pnode->surface->compositor);
	struct gl_surface_state *gs = get_surface_state(pnode->surface);
	/* repaint bounding region in global coordinates: */
	pixman_region32_t repaint;
	/* opaque region in surface coordinates: */
	pixman_region32_t surface_opaque;
	/* non-opaque region in surface coordinates: */
	pixman_region32_t surface_blend;
	GLint filter;
	struct gl_shader_config sconf;

	/* In case of a runtime switch of renderers, we may not have received
	 * an attach for this surface since the switch. In that case we don't
	 * have a valid buffer or a proper shader set up so skip rendering. */
	if (gs->shader_variant == SHADER_VARIANT_NONE && !gs->direct_display)
		return;

	pixman_region32_init(&repaint);
	pixman_region32_intersect(&repaint,
				  &pnode->view->transform.boundingbox, damage);
	pixman_region32_subtract(&repaint, &repaint, &pnode->view->clip);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	if (ensure_surface_buffer_is_ready(gr, gs) < 0)
		goto out;

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	if (pnode->view->transform.enabled || pnode->output->zoom.active ||
	    pnode->output->current_scale != pnode->surface->buffer_viewport.buffer.scale)
		filter = GL_LINEAR;
	else
		filter = GL_NEAREST;

	/* for triangle_fan_debug(), maybe_censor_override() */
	assert(pnode->output->from_sRGB_to_blend == NULL);

	if (!gl_shader_config_init_for_paint_node(&sconf, pnode, filter))
		goto out;

	/* blended region is whole surface minus opaque region: */
	pixman_region32_init_rect(&surface_blend, 0, 0,
				  pnode->surface->width, pnode->surface->height);
	if (pnode->view->geometry.scissor_enabled)
		pixman_region32_intersect(&surface_blend, &surface_blend,
					  &pnode->view->geometry.scissor);
	pixman_region32_subtract(&surface_blend, &surface_blend,
				 &pnode->surface->opaque);

	/* XXX: Should we be using ev->transform.opaque here? */
	pixman_region32_init(&surface_opaque);
	if (pnode->view->geometry.scissor_enabled)
		pixman_region32_intersect(&surface_opaque,
					  &pnode->surface->opaque,
					  &pnode->view->geometry.scissor);
	else
		pixman_region32_copy(&surface_opaque, &pnode->surface->opaque);

	maybe_censor_override(&sconf, pnode->output, pnode->view);

	if (pixman_region32_not_empty(&surface_opaque)) {
		struct gl_shader_config alt = sconf;

		if (alt.req.variant == SHADER_VARIANT_RGBA) {
			/* Special case for RGBA textures with possibly
			 * bad data in alpha channel: use the shader
			 * that forces texture alpha = 1.0.
			 * Xwayland surfaces need this.
			 */
			alt.req.variant = SHADER_VARIANT_RGBX;
		}

		if (pnode->view->alpha < 1.0)
			glEnable(GL_BLEND);
		else
			glDisable(GL_BLEND);

		repaint_region(gr, pnode->view, &repaint, &surface_opaque, &alt);
		gs->used_in_output_repaint = true;
	}

	if (pixman_region32_not_empty(&surface_blend)) {
		glEnable(GL_BLEND);
		repaint_region(gr, pnode->view, &repaint, &surface_blend, &sconf);
		gs->used_in_output_repaint = true;
	}

	pixman_region32_fini(&surface_blend);
	pixman_region32_fini(&surface_opaque);

out:
	pixman_region32_fini(&repaint);
}

static void
repaint_views(struct weston_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *compositor = output->compositor;
	struct weston_paint_node *pnode;

	wl_list_for_each_reverse(pnode, &output->paint_node_z_order_list,
				 z_order_link) {
		if (pnode->view->plane == &compositor->primary_plane)
			draw_paint_node(pnode, damage);
	}
}

static int
gl_renderer_create_fence_fd(struct weston_output *output);

/* Updates the release fences of surfaces that were used in the current output
 * repaint. Should only be used from gl_renderer_repaint_output, so that the
 * information in gl_surface_state.used_in_output_repaint is accurate.
 */
static void
update_buffer_release_fences(struct weston_compositor *compositor,
			     struct weston_output *output)
{
	struct weston_paint_node *pnode;

	wl_list_for_each_reverse(pnode, &output->paint_node_z_order_list,
				 z_order_link) {
		struct weston_view *view = pnode->view;
		struct gl_surface_state *gs;
		struct weston_buffer_release *buffer_release;
		int fence_fd;

		if (view->plane != &compositor->primary_plane)
			continue;

		gs = get_surface_state(view->surface);
		buffer_release = gs->buffer_release_ref.buffer_release;

		if (!gs->used_in_output_repaint || !buffer_release)
			continue;

		fence_fd = gl_renderer_create_fence_fd(output);

		/* If we have a buffer_release then it means we support fences,
		 * and we should be able to create the release fence. If we
		 * can't, something has gone horribly wrong, so disconnect the
		 * client.
		 */
		if (fence_fd == -1) {
			linux_explicit_synchronization_send_server_error(
				buffer_release->resource,
				"Failed to create release fence");
			fd_clear(&buffer_release->fence_fd);
			continue;
		}

		/* At the moment it is safe to just replace the fence_fd,
		 * discarding the previous one:
		 *
		 * 1. If the previous fence fd represents a sync fence from
		 *    a previous repaint cycle, that fence fd is now not
		 *    sufficient to provide the release guarantee and should
		 *    be replaced.
		 *
		 * 2. If the fence fd represents a sync fence from another
		 *    output in the same repaint cycle, it's fine to replace
		 *    it since we are rendering to all outputs using the same
		 *    EGL context, so a fence issued for a later output rendering
		 *    is guaranteed to signal after fences for previous output
		 *    renderings.
		 *
		 * Note that the above is only valid if the buffer_release
		 * fences only originate from the GL renderer, which guarantees
		 * a total order of operations and fences.  If we introduce
		 * fences from other sources (e.g., plane out-fences), we will
		 * need to merge fences instead.
		 */
		fd_update(&buffer_release->fence_fd, fence_fd);
	}
}

static void
draw_output_border_texture(struct gl_renderer *gr,
			   struct gl_output_state *go,
			   struct gl_shader_config *sconf,
			   enum gl_renderer_border_side side,
			   int32_t x, int32_t y,
			   int32_t width, int32_t height)
{
	struct gl_border_image *img = &go->borders[side];
	static GLushort indices [] = { 0, 1, 3, 3, 1, 2 };

	if (!img->data) {
		if (img->tex) {
			glDeleteTextures(1, &img->tex);
			img->tex = 0;
		}

		return;
	}

	if (!img->tex) {
		glGenTextures(1, &img->tex);
		glBindTexture(GL_TEXTURE_2D, img->tex);

		glTexParameteri(GL_TEXTURE_2D,
				GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D,
				GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	} else {
		glBindTexture(GL_TEXTURE_2D, img->tex);
	}

	if (go->border_status & (1 << side)) {
		glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT,
			     img->tex_width, img->height, 0,
			     GL_BGRA_EXT, GL_UNSIGNED_BYTE, img->data);
	}

	sconf->input_tex_filter = GL_NEAREST;
	sconf->input_tex[0] = img->tex;
	gl_renderer_use_program(gr, sconf);

	GLfloat texcoord[] = {
		0.0f, 0.0f,
		(GLfloat)img->width / (GLfloat)img->tex_width, 0.0f,
		(GLfloat)img->width / (GLfloat)img->tex_width, 1.0f,
		0.0f, 1.0f,
	};

	GLfloat verts[] = {
		x, y,
		x + width, y,
		x + width, y + height,
		x, y + height
	};

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texcoord);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);
}

static int
output_has_borders(struct weston_output *output)
{
	struct gl_output_state *go = get_output_state(output);

	return go->borders[GL_RENDERER_BORDER_TOP].data ||
	       go->borders[GL_RENDERER_BORDER_RIGHT].data ||
	       go->borders[GL_RENDERER_BORDER_BOTTOM].data ||
	       go->borders[GL_RENDERER_BORDER_LEFT].data;
}

static void
draw_output_borders(struct weston_output *output,
		    enum gl_border_status border_status)
{
	struct gl_shader_config sconf = {
		.req = {
			.variant = SHADER_VARIANT_RGBA,
		},
		.view_alpha = 1.0f,
	};
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);
	struct gl_border_image *top, *bottom, *left, *right;
	int full_width, full_height;

	if (border_status == BORDER_STATUS_CLEAN)
		return; /* Clean. Nothing to do. */

	assert(output->from_sRGB_to_output == NULL);

	top = &go->borders[GL_RENDERER_BORDER_TOP];
	bottom = &go->borders[GL_RENDERER_BORDER_BOTTOM];
	left = &go->borders[GL_RENDERER_BORDER_LEFT];
	right = &go->borders[GL_RENDERER_BORDER_RIGHT];

	full_width = output->current_mode->width + left->width + right->width;
	full_height = output->current_mode->height + top->height + bottom->height;

	glDisable(GL_BLEND);
	glViewport(0, 0, full_width, full_height);

	weston_matrix_init(&sconf.projection);
	weston_matrix_translate(&sconf.projection,
				-full_width / 2.0, -full_height / 2.0, 0);
	weston_matrix_scale(&sconf.projection,
			    2.0 / full_width, -2.0 / full_height, 1);

	glActiveTexture(GL_TEXTURE0);

	if (border_status & BORDER_TOP_DIRTY)
		draw_output_border_texture(gr, go, &sconf, GL_RENDERER_BORDER_TOP,
					   0, 0,
					   full_width, top->height);
	if (border_status & BORDER_LEFT_DIRTY)
		draw_output_border_texture(gr, go, &sconf, GL_RENDERER_BORDER_LEFT,
					   0, top->height,
					   left->width, output->current_mode->height);
	if (border_status & BORDER_RIGHT_DIRTY)
		draw_output_border_texture(gr, go, &sconf, GL_RENDERER_BORDER_RIGHT,
					   full_width - right->width, top->height,
					   right->width, output->current_mode->height);
	if (border_status & BORDER_BOTTOM_DIRTY)
		draw_output_border_texture(gr, go, &sconf, GL_RENDERER_BORDER_BOTTOM,
					   0, full_height - bottom->height,
					   full_width, bottom->height);
}

static void
output_get_border_damage(struct weston_output *output,
			 enum gl_border_status border_status,
			 pixman_region32_t *damage)
{
	struct gl_output_state *go = get_output_state(output);
	struct gl_border_image *top, *bottom, *left, *right;
	int full_width, full_height;

	if (border_status == BORDER_STATUS_CLEAN)
		return; /* Clean. Nothing to do. */

	top = &go->borders[GL_RENDERER_BORDER_TOP];
	bottom = &go->borders[GL_RENDERER_BORDER_BOTTOM];
	left = &go->borders[GL_RENDERER_BORDER_LEFT];
	right = &go->borders[GL_RENDERER_BORDER_RIGHT];

	full_width = output->current_mode->width + left->width + right->width;
	full_height = output->current_mode->height + top->height + bottom->height;
	if (border_status & BORDER_TOP_DIRTY)
		pixman_region32_union_rect(damage, damage,
					   0, 0,
					   full_width, top->height);
	if (border_status & BORDER_LEFT_DIRTY)
		pixman_region32_union_rect(damage, damage,
					   0, top->height,
					   left->width, output->current_mode->height);
	if (border_status & BORDER_RIGHT_DIRTY)
		pixman_region32_union_rect(damage, damage,
					   full_width - right->width, top->height,
					   right->width, output->current_mode->height);
	if (border_status & BORDER_BOTTOM_DIRTY)
		pixman_region32_union_rect(damage, damage,
					   0, full_height - bottom->height,
					   full_width, bottom->height);
}

static void
output_get_damage(struct weston_output *output,
		  pixman_region32_t *buffer_damage, uint32_t *border_damage)
{
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);
	EGLint buffer_age = 0;
	EGLBoolean ret;
	int i;

	if (gr->has_egl_buffer_age) {
		ret = eglQuerySurface(gr->egl_display, go->egl_surface,
				      EGL_BUFFER_AGE_EXT, &buffer_age);
		if (ret == EGL_FALSE) {
			weston_log("buffer age query failed.\n");
			gl_renderer_print_egl_error_state();
		}
	} else if (go->swap_behavior_is_preserved) {
		buffer_age = 1;
	}

	if (buffer_age == 0 || buffer_age - 1 > BUFFER_DAMAGE_COUNT) {
		pixman_region32_copy(buffer_damage, &output->region);
		*border_damage = BORDER_ALL_DIRTY;
	} else {
		for (i = 0; i < buffer_age - 1; i++)
			*border_damage |= go->border_damage[(go->buffer_damage_index + i) % BUFFER_DAMAGE_COUNT];

		if (*border_damage & BORDER_SIZE_CHANGED) {
			/* If we've had a resize, we have to do a full
			 * repaint. */
			*border_damage |= BORDER_ALL_DIRTY;
			pixman_region32_copy(buffer_damage, &output->region);
		} else {
			for (i = 0; i < buffer_age - 1; i++)
				pixman_region32_union(buffer_damage,
						      buffer_damage,
						      &go->buffer_damage[(go->buffer_damage_index + i) % BUFFER_DAMAGE_COUNT]);
		}
	}
}

static void
output_rotate_damage(struct weston_output *output,
		     pixman_region32_t *output_damage,
		     enum gl_border_status border_status)
{
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);

	if (!gr->has_egl_buffer_age)
		return;

	go->buffer_damage_index += BUFFER_DAMAGE_COUNT - 1;
	go->buffer_damage_index %= BUFFER_DAMAGE_COUNT;

	pixman_region32_copy(&go->buffer_damage[go->buffer_damage_index], output_damage);
	go->border_damage[go->buffer_damage_index] = border_status;
}

/**
 * Given a region in Weston's (top-left-origin) global co-ordinate space,
 * translate it to the co-ordinate space used by GL for our output
 * rendering. This requires shifting it into output co-ordinate space:
 * translating for output offset within the global co-ordinate space,
 * multiplying by output scale to get buffer rather than logical size.
 *
 * Finally, if borders are drawn around the output, we translate the area
 * to account for the border region around the outside, and add any
 * damage if the borders have been redrawn.
 *
 * @param output The output whose co-ordinate space we are after
 * @param global_region The affected region in global co-ordinate space
 * @param[out] rects Y-inverted quads in {x,y,w,h} order; caller must free
 * @param[out] nrects Number of quads (4x number of co-ordinates)
 */
static void
pixman_region_to_egl_y_invert(struct weston_output *output,
			      struct pixman_region32 *global_region,
			      EGLint **rects,
			      EGLint *nrects)
{
	struct gl_output_state *go = get_output_state(output);
	pixman_region32_t transformed;
	struct pixman_box32 *box;
	int buffer_height;
	EGLint *d;
	int i;

	/* Translate from global to output co-ordinate space. */
	pixman_region32_init(&transformed);
	pixman_region32_copy(&transformed, global_region);
	pixman_region32_translate(&transformed, -output->x, -output->y);
	weston_transformed_region(output->width, output->height,
				  output->transform,
				  output->current_scale,
				  &transformed, &transformed);

	/* If we have borders drawn around the output, shift our output damage
	 * to account for borders being drawn around the outside, adding any
	 * damage resulting from borders being redrawn. */
	if (output_has_borders(output)) {
		pixman_region32_translate(&transformed,
					  go->borders[GL_RENDERER_BORDER_LEFT].width,
					  go->borders[GL_RENDERER_BORDER_TOP].height);
		output_get_border_damage(output, go->border_status,
					 &transformed);
	}

	/* Convert from a Pixman region into {x,y,w,h} quads, flipping in the
	 * Y axis to account for GL's lower-left-origin co-ordinate space. */
	box = pixman_region32_rectangles(&transformed, nrects);
	*rects = malloc(*nrects * 4 * sizeof(EGLint));

	buffer_height = go->borders[GL_RENDERER_BORDER_TOP].height +
			output->current_mode->height +
			go->borders[GL_RENDERER_BORDER_BOTTOM].height;

	d = *rects;
	for (i = 0; i < *nrects; ++i) {
		*d++ = box[i].x1;
		*d++ = buffer_height - box[i].y2;
		*d++ = box[i].x2 - box[i].x1;
		*d++ = box[i].y2 - box[i].y1;
	}

	pixman_region32_fini(&transformed);
}

static void
blit_shadow_to_output(struct weston_output *output,
		      pixman_region32_t *output_damage)
{
	struct gl_output_state *go = get_output_state(output);
	const struct gl_shader_config sconf = {
		.req = {
			.variant = SHADER_VARIANT_RGBA,
		},
		.projection = {
			.d = { /* transpose */
				 2.0f,  0.0f, 0.0f, 0.0f,
				 0.0f,  2.0f, 0.0f, 0.0f,
				 0.0f,  0.0f, 1.0f, 0.0f,
				-1.0f, -1.0f, 0.0f, 1.0f
			},
			.type = WESTON_MATRIX_TRANSFORM_SCALE |
				WESTON_MATRIX_TRANSFORM_TRANSLATE,
		},
		.view_alpha = 1.0f,
		.input_tex_filter = GL_NEAREST,
		.input_tex[0] = go->shadow.tex,
	};
	struct gl_renderer *gr = get_renderer(output->compositor);
	double width = output->current_mode->width;
	double height = output->current_mode->height;
	pixman_box32_t *rects;
	int n_rects;
	int i;
	pixman_region32_t translated_damage;
	GLfloat verts[4 * 2];

	assert(output->from_blend_to_output == NULL);

	pixman_region32_init(&translated_damage);

	gl_renderer_use_program(gr, &sconf);
	glDisable(GL_BLEND);

	/* output_damage is in global coordinates */
	pixman_region32_intersect(&translated_damage, output_damage,
				  &output->region);
	/* Convert to output pixel coordinates in-place */
	weston_output_region_from_global(output, &translated_damage);

	rects = pixman_region32_rectangles(&translated_damage, &n_rects);
	for (i = 0; i < n_rects; i++) {

		verts[0] = rects[i].x1 / width;
		verts[1] = (height - rects[i].y1) / height;
		verts[2] = rects[i].x2 / width;
		verts[3] = (height - rects[i].y1) / height;

		verts[4] = rects[i].x2 / width;
		verts[5] = (height - rects[i].y2) / height;
		verts[6] = rects[i].x1 / width;
		verts[7] = (height - rects[i].y2) / height;

		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
		glEnableVertexAttribArray(0);

		/* texcoord: */
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, verts);
		glEnableVertexAttribArray(1);

		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	pixman_region32_fini(&translated_damage);
}

/* NOTE: We now allow falling back to ARGB gl visuals when XRGB is
 * unavailable, so we're assuming the background has no transparency
 * and that everything with a blend, like drop shadows, will have something
 * opaque (like the background) drawn underneath it.
 *
 * Depending on the underlying hardware, violating that assumption could
 * result in seeing through to another display plane.
 */
static void
gl_renderer_repaint_output(struct weston_output *output,
			   pixman_region32_t *output_damage)
{
	struct gl_output_state *go = get_output_state(output);
	struct weston_compositor *compositor = output->compositor;
	struct gl_renderer *gr = get_renderer(compositor);
	EGLBoolean ret;
	static int errored;
	/* areas we've damaged since we last used this buffer */
	pixman_region32_t previous_damage;
	/* total area we need to repaint this time */
	pixman_region32_t total_damage;
	enum gl_border_status border_status = BORDER_STATUS_CLEAN;
	struct weston_paint_node *pnode;

	assert(output->from_blend_to_output_by_backend ||
	       output->from_blend_to_output == NULL);

	if (use_output(output) < 0)
		return;

	/* Clear the used_in_output_repaint flag, so that we can properly track
	 * which surfaces were used in this output repaint. */
	wl_list_for_each_reverse(pnode, &output->paint_node_z_order_list,
				 z_order_link) {
		if (pnode->view->plane == &compositor->primary_plane) {
			struct gl_surface_state *gs =
				get_surface_state(pnode->view->surface);
			gs->used_in_output_repaint = false;
		}
	}

	if (go->begin_render_sync != EGL_NO_SYNC_KHR)
		gr->destroy_sync(gr->egl_display, go->begin_render_sync);
	if (go->end_render_sync != EGL_NO_SYNC_KHR)
		gr->destroy_sync(gr->egl_display, go->end_render_sync);

	go->begin_render_sync = create_render_sync(gr);

	/* Calculate the global GL matrix */
	go->output_matrix = output->matrix;
	weston_matrix_translate(&go->output_matrix,
				-(output->current_mode->width / 2.0),
				-(output->current_mode->height / 2.0), 0);
	weston_matrix_scale(&go->output_matrix,
			    2.0 / output->current_mode->width,
			    -2.0 / output->current_mode->height, 1);

	/* If using shadow, redirect all drawing to it first. */
	if (shadow_exists(go)) {
		/* XXX: Shadow code does not support resizing. */
		assert(output->current_mode->width == go->shadow.width);
		assert(output->current_mode->height == go->shadow.height);

		glBindFramebuffer(GL_FRAMEBUFFER, go->shadow.fbo);
		glViewport(0, 0, go->shadow.width, go->shadow.height);
	} else {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(go->borders[GL_RENDERER_BORDER_LEFT].width,
			   go->borders[GL_RENDERER_BORDER_BOTTOM].height,
			   output->current_mode->width,
			   output->current_mode->height);
	}

	/* In fan debug mode, redraw everything to make sure that we clear any
	 * fans left over from previous draws on this buffer.
	 * This precludes the use of EGL_EXT_swap_buffers_with_damage and
	 * EGL_KHR_partial_update, since we damage the whole area. */
	if (gr->fan_debug) {
		pixman_region32_t undamaged;
		pixman_region32_init(&undamaged);
		pixman_region32_subtract(&undamaged, &output->region,
					 output_damage);
		gr->fan_debug = false;
		repaint_views(output, &undamaged);
		gr->fan_debug = true;
		pixman_region32_fini(&undamaged);
	}

	/* previous_damage covers regions damaged in previous paints since we
	 * last used this buffer */
	pixman_region32_init(&previous_damage);
	pixman_region32_init(&total_damage); /* total area to redraw */

	/* Update previous_damage using buffer_age (if available), and store
	 * current damaged region for future use. */
	output_get_damage(output, &previous_damage, &border_status);
	output_rotate_damage(output, output_damage, go->border_status);

	/* Redraw both areas which have changed since we last used this buffer,
	 * as well as the areas we now want to repaint, to make sure the
	 * buffer is up to date. */
	pixman_region32_union(&total_damage, &previous_damage, output_damage);
	border_status |= go->border_status;

	if (gr->has_egl_partial_update && !gr->fan_debug) {
		int n_egl_rects;
		EGLint *egl_rects;

		/* For partial_update, we need to pass the region which has
		 * changed since we last rendered into this specific buffer;
		 * this is total_damage. */
		pixman_region_to_egl_y_invert(output, &total_damage,
					      &egl_rects, &n_egl_rects);
		gr->set_damage_region(gr->egl_display, go->egl_surface,
				      egl_rects, n_egl_rects);
		free(egl_rects);
	}

	if (shadow_exists(go)) {
		/* Repaint into shadow. */
		if (compositor->test_data.test_quirks.gl_force_full_redraw_of_shadow_fb)
			repaint_views(output, &output->region);
		else
			repaint_views(output, output_damage);

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(go->borders[GL_RENDERER_BORDER_LEFT].width,
			   go->borders[GL_RENDERER_BORDER_BOTTOM].height,
			   output->current_mode->width,
			   output->current_mode->height);
		blit_shadow_to_output(output, &total_damage);
	} else {
		repaint_views(output, &total_damage);
	}

	pixman_region32_fini(&total_damage);
	pixman_region32_fini(&previous_damage);

	draw_output_borders(output, border_status);

	wl_signal_emit(&output->frame_signal, output_damage);

	go->end_render_sync = create_render_sync(gr);

	if (gr->swap_buffers_with_damage && !gr->fan_debug) {
		int n_egl_rects;
		EGLint *egl_rects;

		/* For swap_buffers_with_damage, we need to pass the region
		 * which has changed since the previous SwapBuffers on this
		 * surface - this is output_damage. */
		pixman_region_to_egl_y_invert(output, output_damage,
					      &egl_rects, &n_egl_rects);
		ret = gr->swap_buffers_with_damage(gr->egl_display,
						   go->egl_surface,
						   egl_rects, n_egl_rects);
		free(egl_rects);
	} else {
		ret = eglSwapBuffers(gr->egl_display, go->egl_surface);
	}

	if (ret == EGL_FALSE && !errored) {
		errored = 1;
		weston_log("Failed in eglSwapBuffers.\n");
		gl_renderer_print_egl_error_state();
	}

	go->border_status = BORDER_STATUS_CLEAN;

	/* We have to submit the render sync objects after swap buffers, since
	 * the objects get assigned a valid sync file fd only after a gl flush.
	 */
	timeline_submit_render_sync(gr, output, go->begin_render_sync,
				    TIMELINE_RENDER_POINT_TYPE_BEGIN);
	timeline_submit_render_sync(gr, output, go->end_render_sync,
				    TIMELINE_RENDER_POINT_TYPE_END);

	update_buffer_release_fences(compositor, output);

	gl_renderer_garbage_collect_programs(gr);
}

static int
gl_renderer_read_pixels(struct weston_output *output,
			pixman_format_code_t format, void *pixels,
			uint32_t x, uint32_t y,
			uint32_t width, uint32_t height)
{
	GLenum gl_format;
	struct gl_output_state *go = get_output_state(output);

	x += go->borders[GL_RENDERER_BORDER_LEFT].width;
	y += go->borders[GL_RENDERER_BORDER_BOTTOM].height;

	switch (format) {
	case PIXMAN_a8r8g8b8:
		gl_format = GL_BGRA_EXT;
		break;
	case PIXMAN_a8b8g8r8:
		gl_format = GL_RGBA;
		break;
	default:
		return -1;
	}

	if (use_output(output) < 0)
		return -1;

	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(x, y, width, height, gl_format,
		     GL_UNSIGNED_BYTE, pixels);

	return 0;
}

static GLenum
gl_format_from_internal(GLenum internal_format)
{
	switch (internal_format) {
	case GL_R8_EXT:
		return GL_RED_EXT;
	case GL_RG8_EXT:
		return GL_RG_EXT;
	default:
		return internal_format;
	}
}

static void
gl_renderer_flush_damage(struct weston_surface *surface)
{
	const struct weston_testsuite_quirks *quirks =
		&surface->compositor->test_data.test_quirks;
	struct gl_surface_state *gs = get_surface_state(surface);
	struct weston_buffer *buffer = gs->buffer_ref.buffer;
	struct weston_view *view;
	bool texture_used;
	pixman_box32_t *rectangles;
	uint8_t *data;
	int i, j, n;

	pixman_region32_union(&gs->texture_damage,
			      &gs->texture_damage, &surface->damage);

	if (!buffer)
		return;

	/* Avoid upload, if the texture won't be used this time.
	 * We still accumulate the damage in texture_damage, and
	 * hold the reference to the buffer, in case the surface
	 * migrates back to the primary plane.
	 */
	texture_used = false;
	wl_list_for_each(view, &surface->views, surface_link) {
		if (view->plane == &surface->compositor->primary_plane) {
			texture_used = true;
			break;
		}
	}
	if (!texture_used)
		return;

	if (!pixman_region32_not_empty(&gs->texture_damage) &&
	    !gs->needs_full_upload)
		goto done;

	data = wl_shm_buffer_get_data(buffer->shm_buffer);

	glActiveTexture(GL_TEXTURE0);

	if (gs->needs_full_upload || quirks->gl_force_full_upload) {
		glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0);
		wl_shm_buffer_begin_access(buffer->shm_buffer);
		for (j = 0; j < gs->num_textures; j++) {
			glBindTexture(GL_TEXTURE_2D, gs->textures[j]);
			glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT,
				      gs->pitch / gs->hsub[j]);
			glTexImage2D(GL_TEXTURE_2D, 0,
				     gs->gl_format[j],
				     gs->pitch / gs->hsub[j],
				     buffer->height / gs->vsub[j],
				     0,
				     gl_format_from_internal(gs->gl_format[j]),
				     gs->gl_pixel_type,
				     data + gs->offset[j]);
		}
		wl_shm_buffer_end_access(buffer->shm_buffer);
		goto done;
	}

	rectangles = pixman_region32_rectangles(&gs->texture_damage, &n);
	wl_shm_buffer_begin_access(buffer->shm_buffer);
	for (i = 0; i < n; i++) {
		pixman_box32_t r;

		r = weston_surface_to_buffer_rect(surface, rectangles[i]);

		for (j = 0; j < gs->num_textures; j++) {
			glBindTexture(GL_TEXTURE_2D, gs->textures[j]);
			glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT,
				      gs->pitch / gs->hsub[j]);
			glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT,
				      r.x1 / gs->hsub[j]);
			glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT,
				      r.y1 / gs->hsub[j]);
			glTexSubImage2D(GL_TEXTURE_2D, 0,
					r.x1 / gs->hsub[j],
					r.y1 / gs->vsub[j],
					(r.x2 - r.x1) / gs->hsub[j],
					(r.y2 - r.y1) / gs->vsub[j],
					gl_format_from_internal(gs->gl_format[j]),
					gs->gl_pixel_type,
					data + gs->offset[j]);
		}
	}
	wl_shm_buffer_end_access(buffer->shm_buffer);

done:
	pixman_region32_fini(&gs->texture_damage);
	pixman_region32_init(&gs->texture_damage);
	gs->needs_full_upload = false;

	weston_buffer_reference(&gs->buffer_ref, NULL);
	weston_buffer_release_reference(&gs->buffer_release_ref, NULL);
}

static void
ensure_textures(struct gl_surface_state *gs, GLenum target, int num_textures)
{
	int i;

	if (num_textures <= gs->num_textures) {
		glDeleteTextures(gs->num_textures - num_textures, &gs->textures[num_textures]);
		gs->num_textures = num_textures;
		return;
	}

	glActiveTexture(GL_TEXTURE0);

	for (i = gs->num_textures; i < num_textures; i++) {
		glGenTextures(1, &gs->textures[i]);
		glBindTexture(target, gs->textures[i]);
		glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	gs->num_textures = num_textures;
	glBindTexture(target, 0);
}

static void
gl_renderer_attach_shm(struct weston_surface *es, struct weston_buffer *buffer,
		       struct wl_shm_buffer *shm_buffer)
{
	struct weston_compositor *ec = es->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_surface_state *gs = get_surface_state(es);
	GLenum gl_format[3] = {0, 0, 0};
	GLenum gl_pixel_type;
	int pitch;
	int num_planes;

	buffer->shm_buffer = shm_buffer;
	buffer->width = wl_shm_buffer_get_width(shm_buffer);
	buffer->height = wl_shm_buffer_get_height(shm_buffer);

	num_planes = 1;
	gs->offset[0] = 0;
	gs->hsub[0] = 1;
	gs->vsub[0] = 1;

	switch (wl_shm_buffer_get_format(shm_buffer)) {
	case WL_SHM_FORMAT_XRGB8888:
		gs->shader_variant = SHADER_VARIANT_RGBX;
		pitch = wl_shm_buffer_get_stride(shm_buffer) / 4;
		gl_format[0] = GL_BGRA_EXT;
		gl_pixel_type = GL_UNSIGNED_BYTE;
		es->is_opaque = true;
		break;
	case WL_SHM_FORMAT_ARGB8888:
		gs->shader_variant = SHADER_VARIANT_RGBA;
		pitch = wl_shm_buffer_get_stride(shm_buffer) / 4;
		gl_format[0] = GL_BGRA_EXT;
		gl_pixel_type = GL_UNSIGNED_BYTE;
		es->is_opaque = false;
		break;
	case WL_SHM_FORMAT_RGB565:
		gs->shader_variant = SHADER_VARIANT_RGBX;
		pitch = wl_shm_buffer_get_stride(shm_buffer) / 2;
		gl_format[0] = GL_RGB;
		gl_pixel_type = GL_UNSIGNED_SHORT_5_6_5;
		es->is_opaque = true;
		break;
	case WL_SHM_FORMAT_YUV420:
		gs->shader_variant = SHADER_VARIANT_Y_U_V;
		pitch = wl_shm_buffer_get_stride(shm_buffer);
		gl_pixel_type = GL_UNSIGNED_BYTE;
		num_planes = 3;
		gs->offset[1] = gs->offset[0] + (pitch / gs->hsub[0]) *
				(buffer->height / gs->vsub[0]);
		gs->hsub[1] = 2;
		gs->vsub[1] = 2;
		gs->offset[2] = gs->offset[1] + (pitch / gs->hsub[1]) *
				(buffer->height / gs->vsub[1]);
		gs->hsub[2] = 2;
		gs->vsub[2] = 2;
		if (gr->has_gl_texture_rg) {
			gl_format[0] = GL_R8_EXT;
			gl_format[1] = GL_R8_EXT;
			gl_format[2] = GL_R8_EXT;
		} else {
			gl_format[0] = GL_LUMINANCE;
			gl_format[1] = GL_LUMINANCE;
			gl_format[2] = GL_LUMINANCE;
		}
		es->is_opaque = true;
		break;
	case WL_SHM_FORMAT_NV12:
		pitch = wl_shm_buffer_get_stride(shm_buffer);
		gl_pixel_type = GL_UNSIGNED_BYTE;
		num_planes = 2;
		gs->offset[1] = gs->offset[0] + (pitch / gs->hsub[0]) *
				(buffer->height / gs->vsub[0]);
		gs->hsub[1] = 2;
		gs->vsub[1] = 2;
		if (gr->has_gl_texture_rg) {
			gs->shader_variant = SHADER_VARIANT_Y_UV;
			gl_format[0] = GL_R8_EXT;
			gl_format[1] = GL_RG8_EXT;
		} else {
			gs->shader_variant = SHADER_VARIANT_Y_XUXV;
			gl_format[0] = GL_LUMINANCE;
			gl_format[1] = GL_LUMINANCE_ALPHA;
		}
		es->is_opaque = true;
		break;
	case WL_SHM_FORMAT_YUYV:
		gs->shader_variant = SHADER_VARIANT_Y_XUXV;
		pitch = wl_shm_buffer_get_stride(shm_buffer) / 2;
		gl_pixel_type = GL_UNSIGNED_BYTE;
		num_planes = 2;
		gs->offset[1] = 0;
		gs->hsub[1] = 2;
		gs->vsub[1] = 1;
		if (gr->has_gl_texture_rg)
			gl_format[0] = GL_RG8_EXT;
		else
			gl_format[0] = GL_LUMINANCE_ALPHA;
		gl_format[1] = GL_BGRA_EXT;
		es->is_opaque = true;
		break;
	case WL_SHM_FORMAT_XYUV8888:
		/*
		 * [31:0] X:Y:Cb:Cr 8:8:8:8 little endian
		 *        a:b: g: r in SHADER_VARIANT_XYUV
		 */
		gs->shader_variant = SHADER_VARIANT_XYUV;
		pitch = wl_shm_buffer_get_stride(shm_buffer) / 4;
		gl_format[0] = GL_RGBA;
		gl_pixel_type = GL_UNSIGNED_BYTE;
		es->is_opaque = true;
		break;
	default:
		weston_log("warning: unknown shm buffer format: %08x\n",
			   wl_shm_buffer_get_format(shm_buffer));
		return;
	}

	/* Only allocate a texture if it doesn't match existing one.
	 * If a switch from DRM allocated buffer to a SHM buffer is
	 * happening, we need to allocate a new texture buffer. */
	if (pitch != gs->pitch ||
	    buffer->height != gs->height ||
	    gl_format[0] != gs->gl_format[0] ||
	    gl_format[1] != gs->gl_format[1] ||
	    gl_format[2] != gs->gl_format[2] ||
	    gl_pixel_type != gs->gl_pixel_type ||
	    gs->buffer_type != BUFFER_TYPE_SHM) {
		gs->pitch = pitch;
		gs->height = buffer->height;
		gs->gl_format[0] = gl_format[0];
		gs->gl_format[1] = gl_format[1];
		gs->gl_format[2] = gl_format[2];
		gs->gl_pixel_type = gl_pixel_type;
		gs->buffer_type = BUFFER_TYPE_SHM;
		gs->needs_full_upload = true;
		gs->y_inverted = true;
		gs->direct_display = false;

		gs->surface = es;

		ensure_textures(gs, GL_TEXTURE_2D, num_planes);
	}
}

static void
gl_renderer_attach_egl(struct weston_surface *es, struct weston_buffer *buffer,
		       uint32_t format)
{
	struct weston_compositor *ec = es->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_surface_state *gs = get_surface_state(es);
	EGLint attribs[3];
	GLenum target;
	int i, num_planes;

	buffer->legacy_buffer = (struct wl_buffer *)buffer->resource;
	gr->query_buffer(gr->egl_display, buffer->legacy_buffer,
			 EGL_WIDTH, &buffer->width);
	gr->query_buffer(gr->egl_display, buffer->legacy_buffer,
			 EGL_HEIGHT, &buffer->height);
	gr->query_buffer(gr->egl_display, buffer->legacy_buffer,
			 EGL_WAYLAND_Y_INVERTED_WL, &buffer->y_inverted);

	for (i = 0; i < gs->num_images; i++) {
		egl_image_unref(gs->images[i]);
		gs->images[i] = NULL;
	}
	gs->num_images = 0;
	es->is_opaque = false;
	switch (format) {
	case EGL_TEXTURE_RGB:
		es->is_opaque = true;
		/* fallthrough */
	case EGL_TEXTURE_RGBA:
	default:
		num_planes = 1;
		gs->shader_variant = SHADER_VARIANT_RGBA;
		break;
	case EGL_TEXTURE_EXTERNAL_WL:
		num_planes = 1;
		gs->shader_variant = SHADER_VARIANT_EXTERNAL;
		break;
	case EGL_TEXTURE_Y_UV_WL:
		num_planes = 2;
		gs->shader_variant = SHADER_VARIANT_Y_UV;
		es->is_opaque = true;
		break;
	case EGL_TEXTURE_Y_U_V_WL:
		num_planes = 3;
		gs->shader_variant = SHADER_VARIANT_Y_U_V;
		es->is_opaque = true;
		break;
	case EGL_TEXTURE_Y_XUXV_WL:
		num_planes = 2;
		gs->shader_variant = SHADER_VARIANT_Y_XUXV;
		es->is_opaque = true;
		break;
	}

	target = gl_shader_texture_variant_get_target(gs->shader_variant);
	ensure_textures(gs, target, num_planes);
	for (i = 0; i < num_planes; i++) {
		attribs[0] = EGL_WAYLAND_PLANE_WL;
		attribs[1] = i;
		attribs[2] = EGL_NONE;
		gs->images[i] = egl_image_create(gr,
						 EGL_WAYLAND_BUFFER_WL,
						 buffer->legacy_buffer,
						 attribs);
		if (!gs->images[i]) {
			weston_log("failed to create img for plane %d\n", i);
			continue;
		}
		gs->num_images++;

		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(target, gs->textures[i]);
		gr->image_target_texture_2d(target, gs->images[i]->image);
	}

	gs->pitch = buffer->width;
	gs->height = buffer->height;
	gs->buffer_type = BUFFER_TYPE_EGL;
	gs->y_inverted = buffer->y_inverted;
}

static void
gl_renderer_destroy_dmabuf(struct linux_dmabuf_buffer *dmabuf)
{
	struct dmabuf_image *image = linux_dmabuf_buffer_get_user_data(dmabuf);

	dmabuf_image_destroy(image);
}

static struct egl_image *
import_simple_dmabuf(struct gl_renderer *gr,
                     struct dmabuf_attributes *attributes)
{
	struct egl_image *image;
	EGLint attribs[50];
	int atti = 0;
	bool has_modifier;

	/* This requires the Mesa commit in
	 * Mesa 10.3 (08264e5dad4df448e7718e782ad9077902089a07) or
	 * Mesa 10.2.7 (55d28925e6109a4afd61f109e845a8a51bd17652).
	 * Otherwise Mesa closes the fd behind our back and re-importing
	 * will fail.
	 * https://bugs.freedesktop.org/show_bug.cgi?id=76188
	 */

	attribs[atti++] = EGL_WIDTH;
	attribs[atti++] = attributes->width;
	attribs[atti++] = EGL_HEIGHT;
	attribs[atti++] = attributes->height;
	attribs[atti++] = EGL_LINUX_DRM_FOURCC_EXT;
	attribs[atti++] = attributes->format;

	if (attributes->modifier[0] != DRM_FORMAT_MOD_INVALID) {
		if (!gr->has_dmabuf_import_modifiers)
			return NULL;
		has_modifier = true;
	} else {
		has_modifier = false;
	}

	if (attributes->n_planes > 0) {
		attribs[atti++] = EGL_DMA_BUF_PLANE0_FD_EXT;
		attribs[atti++] = attributes->fd[0];
		attribs[atti++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
		attribs[atti++] = attributes->offset[0];
		attribs[atti++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
		attribs[atti++] = attributes->stride[0];
		if (has_modifier) {
			attribs[atti++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
			attribs[atti++] = attributes->modifier[0] & 0xFFFFFFFF;
			attribs[atti++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
			attribs[atti++] = attributes->modifier[0] >> 32;
		}
	}

	if (attributes->n_planes > 1) {
		attribs[atti++] = EGL_DMA_BUF_PLANE1_FD_EXT;
		attribs[atti++] = attributes->fd[1];
		attribs[atti++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
		attribs[atti++] = attributes->offset[1];
		attribs[atti++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
		attribs[atti++] = attributes->stride[1];
		if (has_modifier) {
			attribs[atti++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
			attribs[atti++] = attributes->modifier[1] & 0xFFFFFFFF;
			attribs[atti++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
			attribs[atti++] = attributes->modifier[1] >> 32;
		}
	}

	if (attributes->n_planes > 2) {
		attribs[atti++] = EGL_DMA_BUF_PLANE2_FD_EXT;
		attribs[atti++] = attributes->fd[2];
		attribs[atti++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
		attribs[atti++] = attributes->offset[2];
		attribs[atti++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
		attribs[atti++] = attributes->stride[2];
		if (has_modifier) {
			attribs[atti++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
			attribs[atti++] = attributes->modifier[2] & 0xFFFFFFFF;
			attribs[atti++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
			attribs[atti++] = attributes->modifier[2] >> 32;
		}
	}

	if (gr->has_dmabuf_import_modifiers) {
		if (attributes->n_planes > 3) {
			attribs[atti++] = EGL_DMA_BUF_PLANE3_FD_EXT;
			attribs[atti++] = attributes->fd[3];
			attribs[atti++] = EGL_DMA_BUF_PLANE3_OFFSET_EXT;
			attribs[atti++] = attributes->offset[3];
			attribs[atti++] = EGL_DMA_BUF_PLANE3_PITCH_EXT;
			attribs[atti++] = attributes->stride[3];
			attribs[atti++] = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT;
			attribs[atti++] = attributes->modifier[3] & 0xFFFFFFFF;
			attribs[atti++] = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT;
			attribs[atti++] = attributes->modifier[3] >> 32;
		}
	}

	attribs[atti++] = EGL_NONE;

	image = egl_image_create(gr, EGL_LINUX_DMA_BUF_EXT, NULL,
				 attribs);

	return image;
}

struct yuv_format_descriptor yuv_formats[] = {
	{
		.format = DRM_FORMAT_YUYV,
		.input_planes = 1,
		.output_planes = 2,
		.texture_type = TEXTURE_Y_XUXV_WL,
		{{
			.width_divisor = 1,
			.height_divisor = 1,
			.format = DRM_FORMAT_GR88,
			.plane_index = 0
		}, {
			.width_divisor = 2,
			.height_divisor = 1,
			.format = DRM_FORMAT_ARGB8888,
			.plane_index = 0
		}}
	}, {
		.format = DRM_FORMAT_NV12,
		.input_planes = 2,
		.output_planes = 2,
		.texture_type = TEXTURE_Y_UV_WL,
		{{
			.width_divisor = 1,
			.height_divisor = 1,
			.format = DRM_FORMAT_R8,
			.plane_index = 0
		}, {
			.width_divisor = 2,
			.height_divisor = 2,
			.format = DRM_FORMAT_GR88,
			.plane_index = 1
		}}
	}, {
		.format = DRM_FORMAT_YUV420,
		.input_planes = 3,
		.output_planes = 3,
		.texture_type = TEXTURE_Y_U_V_WL,
		{{
			.width_divisor = 1,
			.height_divisor = 1,
			.format = DRM_FORMAT_R8,
			.plane_index = 0
		}, {
			.width_divisor = 2,
			.height_divisor = 2,
			.format = DRM_FORMAT_R8,
			.plane_index = 1
		}, {
			.width_divisor = 2,
			.height_divisor = 2,
			.format = DRM_FORMAT_R8,
			.plane_index = 2
		}}
	}, {
		.format = DRM_FORMAT_YUV444,
		.input_planes = 3,
		.output_planes = 3,
		.texture_type = TEXTURE_Y_U_V_WL,
		{{
			.width_divisor = 1,
			.height_divisor = 1,
			.format = DRM_FORMAT_R8,
			.plane_index = 0
		}, {
			.width_divisor = 1,
			.height_divisor = 1,
			.format = DRM_FORMAT_R8,
			.plane_index = 1
		}, {
			.width_divisor = 1,
			.height_divisor = 1,
			.format = DRM_FORMAT_R8,
			.plane_index = 2
		}}
	}, {
		.format = DRM_FORMAT_XYUV8888,
		.input_planes = 1,
		.output_planes = 1,
		.texture_type = TEXTURE_XYUV_WL,
		{{
			.width_divisor = 1,
			.height_divisor = 1,
			.format = DRM_FORMAT_XBGR8888,
			.plane_index = 0
		}}
	}
};

static struct egl_image *
import_dmabuf_single_plane(struct gl_renderer *gr,
                           const struct dmabuf_attributes *attributes,
                           struct yuv_plane_descriptor *descriptor)
{
	struct dmabuf_attributes plane;
	struct egl_image *image;
	char fmt[4];

	plane.width = attributes->width / descriptor->width_divisor;
	plane.height = attributes->height / descriptor->height_divisor;
	plane.format = descriptor->format;
	plane.n_planes = 1;
	plane.fd[0] = attributes->fd[descriptor->plane_index];
	plane.offset[0] = attributes->offset[descriptor->plane_index];
	plane.stride[0] = attributes->stride[descriptor->plane_index];
	plane.modifier[0] = attributes->modifier[descriptor->plane_index];

	image = import_simple_dmabuf(gr, &plane);
	if (!image) {
		weston_log("Failed to import plane %d as %.4s\n",
		           descriptor->plane_index,
		           dump_format(descriptor->format, fmt));
		return NULL;
	}

	return image;
}

static bool
import_yuv_dmabuf(struct gl_renderer *gr,
                  struct dmabuf_image *image)
{
	unsigned i;
	int j;
	int ret;
	struct yuv_format_descriptor *format = NULL;
	struct dmabuf_attributes *attributes = &image->dmabuf->attributes;
	char fmt[4];

	for (i = 0; i < ARRAY_LENGTH(yuv_formats); ++i) {
		if (yuv_formats[i].format == attributes->format) {
			format = &yuv_formats[i];
			break;
		}
	}

	if (!format) {
		weston_log("Error during import, and no known conversion for format "
		           "%.4s in the renderer\n",
		           dump_format(attributes->format, fmt));
		return false;
	}

	if (attributes->n_planes != format->input_planes) {
		weston_log("%.4s dmabuf must contain %d plane%s (%d provided)\n",
		           dump_format(format->format, fmt),
		           format->input_planes,
		           (format->input_planes > 1) ? "s" : "",
		           attributes->n_planes);
		return false;
	}

	for (j = 0; j < format->output_planes; ++j) {
		image->images[j] = import_dmabuf_single_plane(gr, attributes,
		                                              &format->plane[j]);
		if (!image->images[j]) {
			while (j) {
				ret = egl_image_unref(image->images[--j]);
				assert(ret == 0);
			}
			return false;
		}
	}

	image->num_images = format->output_planes;

	switch (format->texture_type) {
	case TEXTURE_Y_XUXV_WL:
		image->shader_variant = SHADER_VARIANT_Y_XUXV;
		break;
	case TEXTURE_Y_UV_WL:
		image->shader_variant = SHADER_VARIANT_Y_UV;
		break;
	case TEXTURE_Y_U_V_WL:
		image->shader_variant = SHADER_VARIANT_Y_U_V;
		break;
	case TEXTURE_XYUV_WL:
		image->shader_variant = SHADER_VARIANT_XYUV;
		break;
	default:
		assert(false);
	}

	return true;
}

static void
gl_renderer_query_dmabuf_modifiers_full(struct gl_renderer *gr, int format,
					uint64_t **modifiers,
					unsigned **external_only,
					int *num_modifiers);

static struct dmabuf_format*
dmabuf_format_create(struct gl_renderer *gr, uint32_t format)
{
	struct dmabuf_format *dmabuf_format;

	dmabuf_format = calloc(1, sizeof(struct dmabuf_format));
	if (!dmabuf_format)
		return NULL;

	dmabuf_format->format = format;

	gl_renderer_query_dmabuf_modifiers_full(gr, format,
			&dmabuf_format->modifiers,
			&dmabuf_format->external_only,
			&dmabuf_format->num_modifiers);

	if (dmabuf_format->num_modifiers == 0) {
		free(dmabuf_format);
		return NULL;
	}

	wl_list_insert(&gr->dmabuf_formats, &dmabuf_format->link);
	return dmabuf_format;
}

static void
dmabuf_format_destroy(struct dmabuf_format *format)
{
	free(format->modifiers);
	free(format->external_only);
	wl_list_remove(&format->link);
	free(format);
}

static GLenum
choose_texture_target(struct gl_renderer *gr,
		      struct dmabuf_attributes *attributes)
{
	struct dmabuf_format *tmp, *format = NULL;

	wl_list_for_each(tmp, &gr->dmabuf_formats, link) {
		if (tmp->format == attributes->format) {
			format = tmp;
			break;
		}
	}

	if (!format)
		format = dmabuf_format_create(gr, attributes->format);

	if (format) {
		int i;

		for (i = 0; i < format->num_modifiers; ++i) {
			if (format->modifiers[i] == attributes->modifier[0]) {
				if(format->external_only[i])
					return GL_TEXTURE_EXTERNAL_OES;
				else
					return GL_TEXTURE_2D;
			}
		}
	}

	if (attributes->n_planes > 1)
		return GL_TEXTURE_EXTERNAL_OES;

	switch (attributes->format & ~DRM_FORMAT_BIG_ENDIAN) {
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_AYUV:
	case DRM_FORMAT_XYUV8888:
		return GL_TEXTURE_EXTERNAL_OES;
	default:
		return GL_TEXTURE_2D;
	}
}

static struct dmabuf_image *
import_dmabuf(struct gl_renderer *gr,
	      struct linux_dmabuf_buffer *dmabuf)
{
	struct egl_image *egl_image;
	struct dmabuf_image *image;
	GLenum target;

	image = dmabuf_image_create();
	image->dmabuf = dmabuf;

	egl_image = import_simple_dmabuf(gr, &dmabuf->attributes);
	if (egl_image) {
		image->num_images = 1;
		image->images[0] = egl_image;
		image->import_type = IMPORT_TYPE_DIRECT;
		target = choose_texture_target(gr, &dmabuf->attributes);

		switch (target) {
		case GL_TEXTURE_2D:
			image->shader_variant = SHADER_VARIANT_RGBA;
			break;
		default:
			image->shader_variant = SHADER_VARIANT_EXTERNAL;
		}
	} else {
		if (!import_yuv_dmabuf(gr, image)) {
			dmabuf_image_destroy(image);
			return NULL;
		}
		image->import_type = IMPORT_TYPE_GL_CONVERSION;
	}

	return image;
}

static void
gl_renderer_query_dmabuf_formats(struct weston_compositor *wc,
				int **formats, int *num_formats)
{
	struct gl_renderer *gr = get_renderer(wc);
	static const int fallback_formats[] = {
		DRM_FORMAT_ARGB8888,
		DRM_FORMAT_XRGB8888,
		DRM_FORMAT_YUYV,
		DRM_FORMAT_NV12,
		DRM_FORMAT_YUV420,
		DRM_FORMAT_YUV444,
		DRM_FORMAT_XYUV8888,
	};
	bool fallback = false;
	EGLint num;

	assert(gr->has_dmabuf_import);

	if (!gr->has_dmabuf_import_modifiers ||
	    !gr->query_dmabuf_formats(gr->egl_display, 0, NULL, &num)) {
		num = gr->has_gl_texture_rg ? ARRAY_LENGTH(fallback_formats) : 2;
		fallback = true;
	}

	*formats = calloc(num, sizeof(int));
	if (*formats == NULL) {
		*num_formats = 0;
		return;
	}

	if (fallback) {
		memcpy(*formats, fallback_formats, num * sizeof(int));
		*num_formats = num;
		return;
	}

	if (!gr->query_dmabuf_formats(gr->egl_display, num, *formats, &num)) {
		*num_formats = 0;
		free(*formats);
		return;
	}

	*num_formats = num;
}

static void
gl_renderer_query_dmabuf_modifiers_full(struct gl_renderer *gr, int format,
					uint64_t **modifiers,
					unsigned **external_only,
					int *num_modifiers)
{
	int num;

	assert(gr->has_dmabuf_import);

	if (!gr->has_dmabuf_import_modifiers ||
		!gr->query_dmabuf_modifiers(gr->egl_display, format, 0, NULL,
					    NULL, &num) ||
		num == 0) {
		*num_modifiers = 0;
		return;
	}

	*modifiers = calloc(num, sizeof(uint64_t));
	if (*modifiers == NULL) {
		*num_modifiers = 0;
		return;
	}
	if (external_only) {
		*external_only = calloc(num, sizeof(unsigned));
		if (*external_only == NULL) {
			*num_modifiers = 0;
			free(*modifiers);
			return;
		}
	}
	if (!gr->query_dmabuf_modifiers(gr->egl_display, format,
				num, *modifiers, external_only ?
				*external_only : NULL, &num)) {
		*num_modifiers = 0;
		free(*modifiers);
		if (external_only)
			free(*external_only);
		return;
	}

	*num_modifiers = num;
}

static void
gl_renderer_query_dmabuf_modifiers(struct weston_compositor *wc, int format,
					uint64_t **modifiers,
					int *num_modifiers)
{
	struct gl_renderer *gr = get_renderer(wc);

	gl_renderer_query_dmabuf_modifiers_full(gr, format, modifiers, NULL,
			num_modifiers);
}

static bool
gl_renderer_import_dmabuf(struct weston_compositor *ec,
			  struct linux_dmabuf_buffer *dmabuf)
{
	struct gl_renderer *gr = get_renderer(ec);
	struct dmabuf_image *image;
	int i;

	assert(gr->has_dmabuf_import);

	for (i = 0; i < dmabuf->attributes.n_planes; i++) {
		/* return if EGL doesn't support import modifiers */
		if (dmabuf->attributes.modifier[i] != DRM_FORMAT_MOD_INVALID)
			if (!gr->has_dmabuf_import_modifiers)
				return false;

		/* return if modifiers passed are unequal */
		if (dmabuf->attributes.modifier[i] !=
		    dmabuf->attributes.modifier[0])
			return false;
	}

	/* reject all flags we do not recognize or handle */
	if (dmabuf->attributes.flags & ~ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT)
		return false;

	image = import_dmabuf(gr, dmabuf);
	if (!image)
		return false;

	wl_list_insert(&gr->dmabuf_images, &image->link);
	linux_dmabuf_buffer_set_user_data(dmabuf, image,
		gl_renderer_destroy_dmabuf);

	return true;
}

static bool
import_known_dmabuf(struct gl_renderer *gr,
                    struct dmabuf_image *image)
{
	switch (image->import_type) {
	case IMPORT_TYPE_DIRECT:
		image->images[0] = import_simple_dmabuf(gr, &image->dmabuf->attributes);
		if (!image->images[0])
			return false;
		image->num_images = 1;
		break;

	case IMPORT_TYPE_GL_CONVERSION:
		if (!import_yuv_dmabuf(gr, image))
			return false;
		break;

	default:
		weston_log("Invalid import type for dmabuf\n");
		return false;
	}

	return true;
}

static bool
dmabuf_is_opaque(struct linux_dmabuf_buffer *dmabuf)
{
	const struct pixel_format_info *info;

	info = pixel_format_get_info(dmabuf->attributes.format &
				     ~DRM_FORMAT_BIG_ENDIAN);
	if (!info)
		return false;

	return pixel_format_is_opaque(info);
}

static void
gl_renderer_attach_dmabuf(struct weston_surface *surface,
			  struct weston_buffer *buffer,
			  struct linux_dmabuf_buffer *dmabuf)
{
	struct gl_renderer *gr = get_renderer(surface->compositor);
	struct gl_surface_state *gs = get_surface_state(surface);
	struct dmabuf_image *image;
	GLenum target;
	int i;

	if (!gr->has_dmabuf_import) {
		linux_dmabuf_buffer_send_server_error(dmabuf,
				"EGL dmabuf import not supported");
		return;
	}

	buffer->width = dmabuf->attributes.width;
	buffer->height = dmabuf->attributes.height;

	/*
	 * GL-renderer uses the OpenGL convention of texture coordinates, where
	 * the origin is at bottom-left. Because dmabuf buffers have the origin
	 * at top-left, we must invert the Y_INVERT flag to get the image right.
	 */
	buffer->y_inverted =
		!(dmabuf->attributes.flags & ZWP_LINUX_BUFFER_PARAMS_V1_FLAGS_Y_INVERT);

	for (i = 0; i < gs->num_images; i++)
		egl_image_unref(gs->images[i]);
	gs->num_images = 0;

	gs->pitch = buffer->width;
	gs->height = buffer->height;
	gs->buffer_type = BUFFER_TYPE_EGL;
	gs->y_inverted = buffer->y_inverted;
	gs->direct_display = dmabuf->direct_display;
	surface->is_opaque = dmabuf_is_opaque(dmabuf);

	/*
	 * We try to always hold an imported EGLImage from the dmabuf
	 * to prevent the client from preventing re-imports. But, we also
	 * need to re-import every time the contents may change because
	 * GL driver's caching may need flushing.
	 *
	 * Here we release the cache reference which has to be final.
	 */
	if (dmabuf->direct_display)
		return;

	image = linux_dmabuf_buffer_get_user_data(dmabuf);

	/* The dmabuf_image should have been created during the import */
	assert(image != NULL);

	for (i = 0; i < image->num_images; ++i)
		egl_image_unref(image->images[i]);

	if (!import_known_dmabuf(gr, image)) {
		linux_dmabuf_buffer_send_server_error(dmabuf, "EGL dmabuf import failed");
		return;
	}

	gs->num_images = image->num_images;
	for (i = 0; i < gs->num_images; ++i)
		gs->images[i] = egl_image_ref(image->images[i]);

	target = gl_shader_texture_variant_get_target(image->shader_variant);
	ensure_textures(gs, target, gs->num_images);
	for (i = 0; i < gs->num_images; ++i) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(target, gs->textures[i]);
		gr->image_target_texture_2d(target, gs->images[i]->image);
	}

	gs->shader_variant = image->shader_variant;
}

static const struct weston_drm_format_array *
gl_renderer_get_supported_formats(struct weston_compositor *ec)
{
	struct gl_renderer *gr = get_renderer(ec);

	return &gr->supported_formats;
}

static int
populate_supported_formats(struct weston_compositor *ec,
			   struct weston_drm_format_array *supported_formats)
{
	struct weston_drm_format *fmt;
	int *formats = NULL;
	uint64_t *modifiers = NULL;
	unsigned int num_formats, num_modifiers;
	unsigned int i, j;
	int ret = 0;

	/* Use EGL_EXT_image_dma_buf_import_modifiers to query the
	 * list of formats/modifiers of the renderer. */
	gl_renderer_query_dmabuf_formats(ec, &formats, &num_formats);
	if (num_formats == 0)
		return 0;

	for (i = 0; i < num_formats; i++) {
		fmt = weston_drm_format_array_add_format(supported_formats,
							 formats[i]);
		if (!fmt) {
			ret = -1;
			goto out;
		}

		/* Always add DRM_FORMAT_MOD_INVALID, as EGL implementations
		 * support implicit modifiers. */
		ret = weston_drm_format_add_modifier(fmt, DRM_FORMAT_MOD_INVALID);
		if (ret < 0)
			goto out;

		gl_renderer_query_dmabuf_modifiers(ec, formats[i],
						   &modifiers, &num_modifiers);
		if (num_modifiers == 0)
			continue;

		for (j = 0; j < num_modifiers; j++) {
			/* Skip MOD_INVALID, as it has already been added. */
			if (modifiers[j] == DRM_FORMAT_MOD_INVALID)
				continue;
			ret = weston_drm_format_add_modifier(fmt, modifiers[j]);
			if (ret < 0) {
				free(modifiers);
				goto out;
			}
		}
		free(modifiers);
	}

out:
	free(formats);
	return ret;
}

static void
gl_renderer_attach(struct weston_surface *es, struct weston_buffer *buffer)
{
	struct weston_compositor *ec = es->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	struct gl_surface_state *gs = get_surface_state(es);
	struct wl_shm_buffer *shm_buffer;
	struct linux_dmabuf_buffer *dmabuf;
	EGLint format;
	int i;

	weston_buffer_reference(&gs->buffer_ref, buffer);
	weston_buffer_release_reference(&gs->buffer_release_ref,
					es->buffer_release_ref.buffer_release);

	if (!buffer) {
		for (i = 0; i < gs->num_images; i++) {
			egl_image_unref(gs->images[i]);
			gs->images[i] = NULL;
		}
		gs->num_images = 0;
		glDeleteTextures(gs->num_textures, gs->textures);
		gs->num_textures = 0;
		gs->buffer_type = BUFFER_TYPE_NULL;
		gs->y_inverted = true;
		gs->direct_display = false;
		es->is_opaque = false;
		return;
	}

	shm_buffer = wl_shm_buffer_get(buffer->resource);

	if (shm_buffer)
		gl_renderer_attach_shm(es, buffer, shm_buffer);
	else if (gr->has_bind_display &&
		 gr->query_buffer(gr->egl_display, (void *)buffer->resource,
				  EGL_TEXTURE_FORMAT, &format))
		gl_renderer_attach_egl(es, buffer, format);
	else if ((dmabuf = linux_dmabuf_buffer_get(buffer->resource)))
		gl_renderer_attach_dmabuf(es, buffer, dmabuf);
	else {
		weston_log("unhandled buffer type!\n");
		if (gr->has_bind_display) {
		  weston_log("eglQueryWaylandBufferWL failed\n");
		  gl_renderer_print_egl_error_state();
		}
		weston_buffer_reference(&gs->buffer_ref, NULL);
		weston_buffer_release_reference(&gs->buffer_release_ref, NULL);
		gs->buffer_type = BUFFER_TYPE_NULL;
		gs->y_inverted = true;
		es->is_opaque = false;
                weston_buffer_send_server_error(buffer,
			"disconnecting due to unhandled buffer type");
	}
}

static void
gl_renderer_surface_set_color(struct weston_surface *surface,
			      float red, float green, float blue, float alpha)
{
	struct gl_surface_state *gs = get_surface_state(surface);

	gs->color[0] = red;
	gs->color[1] = green;
	gs->color[2] = blue;
	gs->color[3] = alpha;
	gs->buffer_type = BUFFER_TYPE_SOLID;
	gs->pitch = 1;
	gs->height = 1;

	gs->shader_variant = SHADER_VARIANT_SOLID;
}

static void
gl_renderer_surface_get_content_size(struct weston_surface *surface,
				     int *width, int *height)
{
	struct gl_surface_state *gs = get_surface_state(surface);

	if (gs->buffer_type == BUFFER_TYPE_NULL) {
		*width = 0;
		*height = 0;
	} else {
		*width = gs->pitch;
		*height = gs->height;
	}
}

static uint32_t
pack_color(pixman_format_code_t format, float *c)
{
	uint8_t r = round(c[0] * 255.0f);
	uint8_t g = round(c[1] * 255.0f);
	uint8_t b = round(c[2] * 255.0f);
	uint8_t a = round(c[3] * 255.0f);

	switch (format) {
	case PIXMAN_a8b8g8r8:
		return (a << 24) | (b << 16) | (g << 8) | r;
	default:
		assert(0);
		return 0;
	}
}

static int
gl_renderer_surface_copy_content(struct weston_surface *surface,
				 void *target, size_t size,
				 int src_x, int src_y,
				 int width, int height)
{
	static const GLfloat verts[4 * 2] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		1.0f, 1.0f,
		0.0f, 1.0f
	};
	static const GLfloat projmat_normal[16] = { /* transpose */
		 2.0f,  0.0f, 0.0f, 0.0f,
		 0.0f,  2.0f, 0.0f, 0.0f,
		 0.0f,  0.0f, 1.0f, 0.0f,
		-1.0f, -1.0f, 0.0f, 1.0f
	};
	static const GLfloat projmat_yinvert[16] = { /* transpose */
		 2.0f,  0.0f, 0.0f, 0.0f,
		 0.0f, -2.0f, 0.0f, 0.0f,
		 0.0f,  0.0f, 1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f, 1.0f
	};
	struct gl_shader_config sconf = {
		.view_alpha = 1.0f,
		.input_tex_filter = GL_NEAREST,
	};
	const pixman_format_code_t format = PIXMAN_a8b8g8r8;
	const size_t bytespp = 4; /* PIXMAN_a8b8g8r8 */
	const GLenum gl_format = GL_RGBA; /* PIXMAN_a8b8g8r8 little-endian */
	struct gl_renderer *gr = get_renderer(surface->compositor);
	struct gl_surface_state *gs = get_surface_state(surface);
	int cw, ch;
	GLuint fbo;
	GLuint tex;
	GLenum status;
	int ret = -1;

	gl_renderer_surface_get_content_size(surface, &cw, &ch);

	switch (gs->buffer_type) {
	case BUFFER_TYPE_NULL:
		return -1;
	case BUFFER_TYPE_SOLID:
		*(uint32_t *)target = pack_color(format, gs->color);
		return 0;
	case BUFFER_TYPE_SHM:
		gl_renderer_flush_damage(surface);
		/* fall through */
	case BUFFER_TYPE_EGL:
		break;
	}

	gl_shader_config_set_input_textures(&sconf, gs);

	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cw, ch,
		     0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, tex, 0);

	status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		weston_log("%s: fbo error: %#x\n", __func__, status);
		goto out;
	}

	glViewport(0, 0, cw, ch);
	glDisable(GL_BLEND);
	if (gs->y_inverted)
		ARRAY_COPY(sconf.projection.d, projmat_normal);
	else
		ARRAY_COPY(sconf.projection.d, projmat_yinvert);
	sconf.projection.type = WESTON_MATRIX_TRANSFORM_SCALE |
				WESTON_MATRIX_TRANSFORM_TRANSLATE;

	if (!gl_renderer_use_program(gr, &sconf))
		goto out;

	/* position: */
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glEnableVertexAttribArray(0);

	/* texcoord: */
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glEnableVertexAttribArray(1);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);

	glPixelStorei(GL_PACK_ALIGNMENT, bytespp);
	glReadPixels(src_x, src_y, width, height, gl_format,
		     GL_UNSIGNED_BYTE, target);
	ret = 0;

out:
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);

	return ret;
}

static void
surface_state_destroy(struct gl_surface_state *gs, struct gl_renderer *gr)
{
	int i;

	wl_list_remove(&gs->surface_destroy_listener.link);
	wl_list_remove(&gs->renderer_destroy_listener.link);

	gs->surface->renderer_state = NULL;

	glDeleteTextures(gs->num_textures, gs->textures);

	for (i = 0; i < gs->num_images; i++)
		egl_image_unref(gs->images[i]);

	weston_buffer_reference(&gs->buffer_ref, NULL);
	weston_buffer_release_reference(&gs->buffer_release_ref, NULL);
	pixman_region32_fini(&gs->texture_damage);
	free(gs);
}

static void
surface_state_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct gl_surface_state *gs;
	struct gl_renderer *gr;

	gs = container_of(listener, struct gl_surface_state,
			  surface_destroy_listener);

	gr = get_renderer(gs->surface->compositor);

	surface_state_destroy(gs, gr);
}

static void
surface_state_handle_renderer_destroy(struct wl_listener *listener, void *data)
{
	struct gl_surface_state *gs;
	struct gl_renderer *gr;

	gr = data;

	gs = container_of(listener, struct gl_surface_state,
			  renderer_destroy_listener);

	surface_state_destroy(gs, gr);
}

static int
gl_renderer_create_surface(struct weston_surface *surface)
{
	struct gl_surface_state *gs;
	struct gl_renderer *gr = get_renderer(surface->compositor);

	gs = zalloc(sizeof *gs);
	if (gs == NULL)
		return -1;

	/* A buffer is never attached to solid color surfaces, yet
	 * they still go through texcoord computations. Do not divide
	 * by zero there.
	 */
	gs->pitch = 1;
	gs->y_inverted = true;
	gs->direct_display = false;

	gs->surface = surface;

	pixman_region32_init(&gs->texture_damage);
	surface->renderer_state = gs;

	gs->surface_destroy_listener.notify =
		surface_state_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &gs->surface_destroy_listener);

	gs->renderer_destroy_listener.notify =
		surface_state_handle_renderer_destroy;
	wl_signal_add(&gr->destroy_signal,
		      &gs->renderer_destroy_listener);

	if (surface->buffer_ref.buffer) {
		gl_renderer_attach(surface, surface->buffer_ref.buffer);
		gl_renderer_flush_damage(surface);
	}

	return 0;
}

void
gl_renderer_log_extensions(const char *name, const char *extensions)
{
	const char *p, *end;
	int l;
	int len;

	l = weston_log("%s:", name);
	p = extensions;
	while (*p) {
		end = strchrnul(p, ' ');
		len = end - p;
		if (l + len > 78)
			l = weston_log_continue("\n" STAMP_SPACE "%.*s",
						len, p);
		else
			l += weston_log_continue(" %.*s", len, p);
		for (p = end; isspace(*p); p++)
			;
	}
	weston_log_continue("\n");
}

static void
log_egl_info(EGLDisplay egldpy)
{
	const char *str;

	str = eglQueryString(egldpy, EGL_VERSION);
	weston_log("EGL version: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_VENDOR);
	weston_log("EGL vendor: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_CLIENT_APIS);
	weston_log("EGL client APIs: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_EXTENSIONS);
	gl_renderer_log_extensions("EGL extensions", str ? str : "(null)");
}

static void
log_gl_info(void)
{
	const char *str;

	str = (char *)glGetString(GL_VERSION);
	weston_log("GL version: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	weston_log("GLSL version: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_VENDOR);
	weston_log("GL vendor: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_RENDERER);
	weston_log("GL renderer: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_EXTENSIONS);
	gl_renderer_log_extensions("GL extensions", str ? str : "(null)");
}

static void
gl_renderer_output_set_border(struct weston_output *output,
			      enum gl_renderer_border_side side,
			      int32_t width, int32_t height,
			      int32_t tex_width, unsigned char *data)
{
	struct gl_output_state *go = get_output_state(output);

	if (go->borders[side].width != width ||
	    go->borders[side].height != height)
		/* In this case, we have to blow everything and do a full
		 * repaint. */
		go->border_status |= BORDER_SIZE_CHANGED | BORDER_ALL_DIRTY;

	if (data == NULL) {
		width = 0;
		height = 0;
	}

	go->borders[side].width = width;
	go->borders[side].height = height;
	go->borders[side].tex_width = tex_width;
	go->borders[side].data = data;
	go->border_status |= 1 << side;
}

static int
gl_renderer_setup(struct weston_compositor *ec, EGLSurface egl_surface);

static EGLSurface
gl_renderer_create_window_surface(struct gl_renderer *gr,
				  EGLNativeWindowType window_for_legacy,
				  void *window_for_platform,
				  const uint32_t *drm_formats,
				  unsigned drm_formats_count)
{
	EGLSurface egl_surface = EGL_NO_SURFACE;
	EGLConfig egl_config;

	egl_config = gl_renderer_get_egl_config(gr, EGL_WINDOW_BIT,
						drm_formats, drm_formats_count);
	if (egl_config == EGL_NO_CONFIG_KHR)
		return EGL_NO_SURFACE;

	log_egl_config_info(gr->egl_display, egl_config);

	if (gr->create_platform_window)
		egl_surface = gr->create_platform_window(gr->egl_display,
							 egl_config,
							 window_for_platform,
							 NULL);
	else
		egl_surface = eglCreateWindowSurface(gr->egl_display,
						     egl_config,
						     window_for_legacy, NULL);

	return egl_surface;
}

static int
gl_renderer_output_create(struct weston_output *output,
			  EGLSurface surface)
{
	struct gl_output_state *go;
	struct gl_renderer *gr = get_renderer(output->compositor);
	const struct weston_testsuite_quirks *quirks;
	bool ret;
	int i;

	quirks = &output->compositor->test_data.test_quirks;

	go = zalloc(sizeof *go);
	if (go == NULL)
		return -1;

	go->egl_surface = surface;

	for (i = 0; i < BUFFER_DAMAGE_COUNT; i++)
		pixman_region32_init(&go->buffer_damage[i]);

	wl_list_init(&go->timeline_render_point_list);

	go->begin_render_sync = EGL_NO_SYNC_KHR;
	go->end_render_sync = EGL_NO_SYNC_KHR;

	if ((output->from_blend_to_output != NULL &&
	     output->from_blend_to_output_by_backend == false) ||
	    quirks->gl_force_full_redraw_of_shadow_fb) {
		assert(gr->gl_supports_color_transforms);

		ret = gl_fbo_texture_init(&go->shadow,
					  output->current_mode->width,
					  output->current_mode->height,
					  GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT);
		if (ret) {
			weston_log("Output %s uses 16F shadow.\n",
				   output->name);
		} else {
			weston_log("Output %s failed to create 16F shadow.\n",
				   output->name);
			free(go);
			return -1;
		}
	}

	output->renderer_state = go;

	return 0;
}

static int
gl_renderer_output_window_create(struct weston_output *output,
				 const struct gl_renderer_output_options *options)
{
	struct weston_compositor *ec = output->compositor;
	struct gl_renderer *gr = get_renderer(ec);
	EGLSurface egl_surface = EGL_NO_SURFACE;
	int ret = 0;

	egl_surface = gl_renderer_create_window_surface(gr,
							options->window_for_legacy,
							options->window_for_platform,
							options->drm_formats,
							options->drm_formats_count);
	if (egl_surface == EGL_NO_SURFACE) {
		weston_log("failed to create egl surface\n");
		return -1;
	}

	ret = gl_renderer_output_create(output, egl_surface);
	if (ret < 0)
		weston_platform_destroy_egl_surface(gr->egl_display, egl_surface);

	return ret;
}

static int
gl_renderer_output_pbuffer_create(struct weston_output *output,
				  const struct gl_renderer_pbuffer_options *options)
{
	struct gl_renderer *gr = get_renderer(output->compositor);
	struct gl_output_state *go;
	EGLConfig pbuffer_config;
	EGLSurface egl_surface;
	EGLint value = 0;
	int ret;
	EGLint pbuffer_attribs[] = {
		EGL_WIDTH, options->width,
		EGL_HEIGHT, options->height,
		EGL_NONE
	};

	pbuffer_config = gl_renderer_get_egl_config(gr, EGL_PBUFFER_BIT,
						    options->drm_formats,
						    options->drm_formats_count);
	if (pbuffer_config == EGL_NO_CONFIG_KHR) {
		weston_log("failed to choose EGL config for PbufferSurface\n");
		return -1;
	}

	log_egl_config_info(gr->egl_display, pbuffer_config);

	egl_surface = eglCreatePbufferSurface(gr->egl_display, pbuffer_config,
					      pbuffer_attribs);
	if (egl_surface == EGL_NO_SURFACE) {
		weston_log("failed to create egl surface\n");
		gl_renderer_print_egl_error_state();
		return -1;
	}

	eglSurfaceAttrib(gr->egl_display, egl_surface,
			 EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
	if (!eglQuerySurface(gr->egl_display, egl_surface,
			     EGL_SWAP_BEHAVIOR, &value) ||
	    value != EGL_BUFFER_PRESERVED) {
		weston_log("Error: pbuffer surface does not support EGL_BUFFER_PRESERVED, got 0x%x."
			   " Continuing anyway.\n", value);
	}

	ret = gl_renderer_output_create(output, egl_surface);
	if (ret < 0) {
		eglDestroySurface(gr->egl_display, egl_surface);
	} else {
		go = get_output_state(output);
		go->swap_behavior_is_preserved = true;
	}

	return ret;
}

static void
gl_renderer_output_destroy(struct weston_output *output)
{
	struct gl_renderer *gr = get_renderer(output->compositor);
	struct gl_output_state *go = get_output_state(output);
	struct timeline_render_point *trp, *tmp;
	int i;

	for (i = 0; i < 2; i++)
		pixman_region32_fini(&go->buffer_damage[i]);

	if (shadow_exists(go))
		gl_fbo_texture_fini(&go->shadow);

	eglMakeCurrent(gr->egl_display,
		       gr->dummy_surface, gr->dummy_surface, gr->egl_context);

	weston_platform_destroy_egl_surface(gr->egl_display, go->egl_surface);

	if (!wl_list_empty(&go->timeline_render_point_list))
		weston_log("warning: discarding pending timeline render"
			   "objects at output destruction");

	wl_list_for_each_safe(trp, tmp, &go->timeline_render_point_list, link)
		timeline_render_point_destroy(trp);

	if (go->begin_render_sync != EGL_NO_SYNC_KHR)
		gr->destroy_sync(gr->egl_display, go->begin_render_sync);
	if (go->end_render_sync != EGL_NO_SYNC_KHR)
		gr->destroy_sync(gr->egl_display, go->end_render_sync);

	free(go);
}

static int
gl_renderer_create_fence_fd(struct weston_output *output)
{
	struct gl_output_state *go = get_output_state(output);
	struct gl_renderer *gr = get_renderer(output->compositor);
	int fd;

	if (go->end_render_sync == EGL_NO_SYNC_KHR)
		return -1;

	fd = gr->dup_native_fence_fd(gr->egl_display, go->end_render_sync);
	if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID)
		return -1;

	return fd;
}

static void
gl_renderer_destroy(struct weston_compositor *ec)
{
	struct gl_renderer *gr = get_renderer(ec);
	struct dmabuf_image *image, *next;
	struct dmabuf_format *format, *next_format;

	wl_signal_emit(&gr->destroy_signal, gr);

	if (gr->has_bind_display)
		gr->unbind_display(gr->egl_display, ec->wl_display);

	gl_renderer_shader_list_destroy(gr);
	if (gr->fallback_shader)
		gl_shader_destroy(gr, gr->fallback_shader);

	/* Work around crash in egl_dri2.c's dri2_make_current() - when does this apply? */
	eglMakeCurrent(gr->egl_display,
		       EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);

	wl_list_for_each_safe(image, next, &gr->dmabuf_images, link)
		dmabuf_image_destroy(image);

	wl_list_for_each_safe(format, next_format, &gr->dmabuf_formats, link)
		dmabuf_format_destroy(format);

	weston_drm_format_array_fini(&gr->supported_formats);

	if (gr->dummy_surface != EGL_NO_SURFACE)
		weston_platform_destroy_egl_surface(gr->egl_display,
						    gr->dummy_surface);

	eglTerminate(gr->egl_display);
	eglReleaseThread();

	wl_array_release(&gr->vertices);
	wl_array_release(&gr->vtxcnt);

	if (gr->fragment_binding)
		weston_binding_destroy(gr->fragment_binding);
	if (gr->fan_binding)
		weston_binding_destroy(gr->fan_binding);

	weston_log_scope_destroy(gr->shader_scope);
	free(gr);
}

static int
gl_renderer_create_pbuffer_surface(struct gl_renderer *gr) {
	EGLConfig pbuffer_config;
	static const EGLint pbuffer_attribs[] = {
		EGL_WIDTH, 10,
		EGL_HEIGHT, 10,
		EGL_NONE
	};

	pbuffer_config = gr->egl_config;
	if (pbuffer_config == EGL_NO_CONFIG_KHR) {
		pbuffer_config =
			gl_renderer_get_egl_config(gr, EGL_PBUFFER_BIT,
						   NULL, 0);
	}
	if (pbuffer_config == EGL_NO_CONFIG_KHR) {
		weston_log("failed to choose EGL config for PbufferSurface\n");
		return -1;
	}

	gr->dummy_surface = eglCreatePbufferSurface(gr->egl_display,
						    pbuffer_config,
						    pbuffer_attribs);

	if (gr->dummy_surface == EGL_NO_SURFACE) {
		weston_log("failed to create PbufferSurface\n");
		return -1;
	}

	return 0;
}

static int
gl_renderer_display_create(struct weston_compositor *ec,
			   const struct gl_renderer_display_options *options)
{
	struct gl_renderer *gr;
	int ret;

	gr = zalloc(sizeof *gr);
	if (gr == NULL)
		return -1;

	gr->compositor = ec;
	wl_list_init(&gr->shader_list);
	gr->platform = options->egl_platform;

	gr->shader_scope = gl_shader_scope_create(gr);
	if (!gr->shader_scope)
		goto fail;

	if (gl_renderer_setup_egl_client_extensions(gr) < 0)
		goto fail;

	gr->base.read_pixels = gl_renderer_read_pixels;
	gr->base.repaint_output = gl_renderer_repaint_output;
	gr->base.flush_damage = gl_renderer_flush_damage;
	gr->base.attach = gl_renderer_attach;
	gr->base.surface_set_color = gl_renderer_surface_set_color;
	gr->base.destroy = gl_renderer_destroy;
	gr->base.surface_get_content_size =
		gl_renderer_surface_get_content_size;
	gr->base.surface_copy_content = gl_renderer_surface_copy_content;

	if (gl_renderer_setup_egl_display(gr, options->egl_native_display) < 0)
		goto fail;

	weston_drm_format_array_init(&gr->supported_formats);

	log_egl_info(gr->egl_display);

	ec->renderer = &gr->base;

	if (gl_renderer_setup_egl_extensions(ec) < 0)
		goto fail_with_error;

	if (!gr->has_configless_context) {
		EGLint egl_surface_type = options->egl_surface_type;

		if (!gr->has_surfaceless_context)
			egl_surface_type |= EGL_PBUFFER_BIT;

		gr->egl_config =
			gl_renderer_get_egl_config(gr,
						   egl_surface_type,
						   options->drm_formats,
						   options->drm_formats_count);
		if (gr->egl_config == EGL_NO_CONFIG_KHR) {
			weston_log("failed to choose EGL config\n");
			goto fail_terminate;
		}
	}

	ec->capabilities |= WESTON_CAP_ROTATION_ANY;
	ec->capabilities |= WESTON_CAP_CAPTURE_YFLIP;
	ec->capabilities |= WESTON_CAP_VIEW_CLIP_MASK;
	if (gr->has_native_fence_sync && gr->has_wait_sync)
		ec->capabilities |= WESTON_CAP_EXPLICIT_SYNC;

	wl_list_init(&gr->dmabuf_images);
	if (gr->has_dmabuf_import) {
		gr->base.import_dmabuf = gl_renderer_import_dmabuf;
		gr->base.get_supported_formats = gl_renderer_get_supported_formats;
		ret = populate_supported_formats(ec, &gr->supported_formats);
		if (ret < 0)
			goto fail_terminate;
	}
	wl_list_init(&gr->dmabuf_formats);

	if (gr->has_surfaceless_context) {
		weston_log("EGL_KHR_surfaceless_context available\n");
		gr->dummy_surface = EGL_NO_SURFACE;
	} else {
		weston_log("EGL_KHR_surfaceless_context unavailable. "
			   "Trying PbufferSurface\n");

		if (gl_renderer_create_pbuffer_surface(gr) < 0)
			goto fail_with_error;
	}

	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_RGB565);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_YUV420);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_NV12);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_YUYV);
	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_XYUV8888);

	wl_signal_init(&gr->destroy_signal);

	if (gl_renderer_setup(ec, gr->dummy_surface) < 0) {
		if (gr->dummy_surface != EGL_NO_SURFACE)
			weston_platform_destroy_egl_surface(gr->egl_display,
							    gr->dummy_surface);
		goto fail_with_error;
	}

	if (gr->gl_supports_color_transforms)
		ec->capabilities |= WESTON_CAP_COLOR_OPS;

	return 0;

fail_with_error:
	gl_renderer_print_egl_error_state();
fail_terminate:
	weston_drm_format_array_fini(&gr->supported_formats);
	eglTerminate(gr->egl_display);
fail:
	weston_log_scope_destroy(gr->shader_scope);
	free(gr);
	ec->renderer = NULL;
	return -1;
}

static void
fragment_debug_binding(struct weston_keyboard *keyboard,
		       const struct timespec *time,
		       uint32_t key, void *data)
{
	struct weston_compositor *ec = data;
	struct gl_renderer *gr = get_renderer(ec);
	struct weston_output *output;

	gr->fragment_shader_debug = !gr->fragment_shader_debug;

	wl_list_for_each(output, &ec->output_list, link)
		weston_output_damage(output);
}

static void
fan_debug_repaint_binding(struct weston_keyboard *keyboard,
			  const struct timespec *time,
			  uint32_t key, void *data)
{
	struct weston_compositor *compositor = data;
	struct gl_renderer *gr = get_renderer(compositor);

	gr->fan_debug = !gr->fan_debug;
	weston_compositor_damage_all(compositor);
}

static uint32_t
get_gl_version(void)
{
	const char *version;
	int major, minor;

	version = (const char *) glGetString(GL_VERSION);
	if (version &&
	    (sscanf(version, "%d.%d", &major, &minor) == 2 ||
	     sscanf(version, "OpenGL ES %d.%d", &major, &minor) == 2) &&
	    major > 0 && minor >= 0) {
		return gr_gl_version(major, minor);
	}

	weston_log("warning: failed to detect GLES version, defaulting to 2.0.\n");
	return gr_gl_version(2, 0);
}

static int
gl_renderer_setup(struct weston_compositor *ec, EGLSurface egl_surface)
{
	struct gl_renderer *gr = get_renderer(ec);
	const char *extensions;
	EGLBoolean ret;

	EGLint context_attribs[16] = {
		EGL_CONTEXT_CLIENT_VERSION, 0,
	};
	unsigned int nattr = 2;

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		weston_log("failed to bind EGL_OPENGL_ES_API\n");
		gl_renderer_print_egl_error_state();
		return -1;
	}

	/*
	 * Being the compositor we require minimum output latency,
	 * so request a high priority context for ourselves - that should
	 * reschedule all of our rendering and its dependencies to be completed
	 * first. If the driver doesn't permit us to create a high priority
	 * context, it will fallback to the default priority (MEDIUM).
	 */
	if (gr->has_context_priority) {
		context_attribs[nattr++] = EGL_CONTEXT_PRIORITY_LEVEL_IMG;
		context_attribs[nattr++] = EGL_CONTEXT_PRIORITY_HIGH_IMG;
	}

	assert(nattr < ARRAY_LENGTH(context_attribs));
	context_attribs[nattr] = EGL_NONE;

	/* try to create an OpenGLES 3 context first */
	context_attribs[1] = 3;
	gr->egl_context = eglCreateContext(gr->egl_display, gr->egl_config,
					   EGL_NO_CONTEXT, context_attribs);
	if (gr->egl_context == NULL) {
		/* and then fallback to OpenGLES 2 */
		context_attribs[1] = 2;
		gr->egl_context = eglCreateContext(gr->egl_display,
						   gr->egl_config,
						   EGL_NO_CONTEXT,
						   context_attribs);
		if (gr->egl_context == NULL) {
			weston_log("failed to create context\n");
			gl_renderer_print_egl_error_state();
			return -1;
		}
	}

	if (gr->has_context_priority) {
		EGLint value = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;

		eglQueryContext(gr->egl_display, gr->egl_context,
				EGL_CONTEXT_PRIORITY_LEVEL_IMG, &value);

		if (value != EGL_CONTEXT_PRIORITY_HIGH_IMG) {
			weston_log("Failed to obtain a high priority context.\n");
			/* Not an error, continue on as normal */
		}
	}

	ret = eglMakeCurrent(gr->egl_display, egl_surface,
			     egl_surface, gr->egl_context);
	if (ret == EGL_FALSE) {
		weston_log("Failed to make EGL context current.\n");
		gl_renderer_print_egl_error_state();
		return -1;
	}

	gr->gl_version = get_gl_version();
	log_gl_info();

	gr->image_target_texture_2d =
		(void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");

	extensions = (const char *) glGetString(GL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving GL extension string failed.\n");
		return -1;
	}

	if (!weston_check_egl_extension(extensions, "GL_EXT_texture_format_BGRA8888")) {
		weston_log("GL_EXT_texture_format_BGRA8888 not available\n");
		return -1;
	}

	if (weston_check_egl_extension(extensions, "GL_EXT_read_format_bgra"))
		ec->read_format = PIXMAN_a8r8g8b8;
	else
		ec->read_format = PIXMAN_a8b8g8r8;

	if (gr->gl_version < gr_gl_version(3, 0) &&
	    !weston_check_egl_extension(extensions, "GL_EXT_unpack_subimage")) {
		weston_log("GL_EXT_unpack_subimage not available.\n");
		return -1;
	}

	if (gr->gl_version >= gr_gl_version(3, 0) ||
	    weston_check_egl_extension(extensions, "GL_EXT_texture_rg"))
		gr->has_gl_texture_rg = true;

	if (weston_check_egl_extension(extensions, "GL_OES_EGL_image_external"))
		gr->has_egl_image_external = true;

	if (gr->gl_version >= gr_gl_version(3, 0) &&
	    weston_check_egl_extension(extensions, "GL_EXT_color_buffer_half_float")) {
		gr->gl_supports_color_transforms = true;
	}

	glActiveTexture(GL_TEXTURE0);

	gr->fallback_shader = gl_renderer_create_fallback_shader(gr);
	if (!gr->fallback_shader) {
		weston_log("Error: compiling fallback shader failed.\n");
		return -1;
	}

	gr->fragment_binding =
		weston_compositor_add_debug_binding(ec, KEY_S,
						    fragment_debug_binding,
						    ec);
	gr->fan_binding =
		weston_compositor_add_debug_binding(ec, KEY_F,
						    fan_debug_repaint_binding,
						    ec);

	weston_log("GL ES %d.%d - renderer features:\n",
		   gr_gl_version_major(gr->gl_version),
		   gr_gl_version_minor(gr->gl_version));
	weston_log_continue(STAMP_SPACE "read-back format: %s\n",
		ec->read_format == PIXMAN_a8r8g8b8 ? "BGRA" : "RGBA");
	weston_log_continue(STAMP_SPACE "EGL Wayland extension: %s\n",
			    gr->has_bind_display ? "yes" : "no");

	return 0;
}

WL_EXPORT struct gl_renderer_interface gl_renderer_interface = {
	.display_create = gl_renderer_display_create,
	.output_window_create = gl_renderer_output_window_create,
	.output_pbuffer_create = gl_renderer_output_pbuffer_create,
	.output_destroy = gl_renderer_output_destroy,
	.output_set_border = gl_renderer_output_set_border,
	.create_fence_fd = gl_renderer_create_fence_fd,
};
