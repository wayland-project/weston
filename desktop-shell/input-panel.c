/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 * Copyright © 2013 Raspberry Pi Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shell.h"
#include "weston-input-panel-server-protocol.h"
#include "shared/helpers.h"

struct input_panel_surface {
	struct wl_resource *resource;
	struct wl_signal destroy_signal;

	struct desktop_shell *shell;

	struct wl_list link;
	struct weston_surface *surface;
	struct weston_view *view;
	struct wl_listener surface_destroy_listener;

	struct weston_view_animation *anim;

	struct weston_output *output;
	uint32_t panel;
};

static void
input_panel_slide_done(struct weston_view_animation *animation, void *data)
{
	struct input_panel_surface *ipsurf = data;

	ipsurf->anim = NULL;
}

static void
show_input_panel_surface(struct input_panel_surface *ipsurf)
{
	struct desktop_shell *shell = ipsurf->shell;
	struct weston_seat *seat;
	struct weston_surface *focus;
	float x, y;

	wl_list_for_each(seat, &shell->compositor->seat_list, link) {
		struct weston_keyboard *keyboard =
			weston_seat_get_keyboard(seat);

		if (!keyboard || !keyboard->focus)
			continue;
		focus = weston_surface_get_main_surface(keyboard->focus);
		if (!focus)
			continue;
		ipsurf->output = focus->output;
		x = ipsurf->output->x + (ipsurf->output->width - ipsurf->surface->width) / 2;
		y = ipsurf->output->y + ipsurf->output->height - ipsurf->surface->height;
		weston_view_set_position(ipsurf->view, x, y);
	}

	weston_layer_entry_insert(&shell->input_panel_layer.view_list,
	                          &ipsurf->view->layer_link);
	weston_view_geometry_dirty(ipsurf->view);
	weston_view_update_transform(ipsurf->view);
	ipsurf->surface->is_mapped = true;
	ipsurf->view->is_mapped = true;
	weston_surface_damage(ipsurf->surface);

	if (ipsurf->anim)
		weston_view_animation_destroy(ipsurf->anim);

	ipsurf->anim =
		weston_slide_run(ipsurf->view,
				 ipsurf->surface->height * 0.9, 0,
				 input_panel_slide_done, ipsurf);
}

static void
show_input_panels(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell,
			     show_input_panel_listener);
	struct input_panel_surface *ipsurf, *next;

	shell->text_input.surface = (struct weston_surface*)data;

	if (shell->showing_input_panels)
		return;

	shell->showing_input_panels = true;

	if (!shell->locked)
		weston_layer_set_position(&shell->input_panel_layer,
					  WESTON_LAYER_POSITION_TOP_UI);

	wl_list_for_each_safe(ipsurf, next,
			      &shell->input_panel.surfaces, link) {
		if (ipsurf->surface->width == 0)
			continue;

		show_input_panel_surface(ipsurf);
	}
}

static void
hide_input_panels(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell,
			     hide_input_panel_listener);
	struct weston_view *view, *next;

	if (!shell->showing_input_panels)
		return;

	shell->showing_input_panels = false;

	if (!shell->locked)
		weston_layer_unset_position(&shell->input_panel_layer);

	wl_list_for_each_safe(view, next,
			      &shell->input_panel_layer.view_list.link,
			      layer_link.link)
		weston_view_unmap(view);
}

static void
update_input_panels(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell,
			     update_input_panel_listener);

	memcpy(&shell->text_input.cursor_rectangle, data, sizeof(pixman_box32_t));
}

static int
input_panel_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	return snprintf(buf, len, "input panel");
}

static void
input_panel_committed(struct weston_surface *surface, int32_t sx, int32_t sy)
{
	struct input_panel_surface *ip_surface = surface->committed_private;
	struct desktop_shell *shell = ip_surface->shell;
	struct weston_view *view;
	float x, y;

	if (surface->width == 0)
		return;

	if (ip_surface->panel) {
		view = get_default_view(shell->text_input.surface);
		if (view == NULL)
			return;
		x = view->geometry.x + shell->text_input.cursor_rectangle.x2;
		y = view->geometry.y + shell->text_input.cursor_rectangle.y2;
	} else {
		x = ip_surface->output->x + (ip_surface->output->width - surface->width) / 2;
		y = ip_surface->output->y + ip_surface->output->height - surface->height;
	}

	weston_view_set_position(ip_surface->view, x, y);

	if (!weston_surface_is_mapped(surface) && shell->showing_input_panels)
		show_input_panel_surface(ip_surface);
}

static void
destroy_input_panel_surface(struct input_panel_surface *input_panel_surface)
{
	wl_signal_emit(&input_panel_surface->destroy_signal, input_panel_surface);

	wl_list_remove(&input_panel_surface->surface_destroy_listener.link);
	wl_list_remove(&input_panel_surface->link);

	input_panel_surface->surface->committed = NULL;
	weston_surface_set_label_func(input_panel_surface->surface, NULL);
	weston_view_destroy(input_panel_surface->view);

	free(input_panel_surface);
}

static struct input_panel_surface *
get_input_panel_surface(struct weston_surface *surface)
{
	if (surface->committed == input_panel_committed) {
		return surface->committed_private;
	} else {
		return NULL;
	}
}

static void
input_panel_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct input_panel_surface *ipsurface = container_of(listener,
							     struct input_panel_surface,
							     surface_destroy_listener);

	if (ipsurface->resource) {
		wl_resource_destroy(ipsurface->resource);
	} else {
		destroy_input_panel_surface(ipsurface);
	}
}

static struct input_panel_surface *
create_input_panel_surface(struct desktop_shell *shell,
			   struct weston_surface *surface)
{
	struct input_panel_surface *input_panel_surface;

	input_panel_surface = calloc(1, sizeof *input_panel_surface);
	if (!input_panel_surface)
		return NULL;

	surface->committed = input_panel_committed;
	surface->committed_private = input_panel_surface;
	weston_surface_set_label_func(surface, input_panel_get_label);

	input_panel_surface->shell = shell;

	input_panel_surface->surface = surface;
	input_panel_surface->view = weston_view_create(surface);

	wl_signal_init(&input_panel_surface->destroy_signal);
	input_panel_surface->surface_destroy_listener.notify = input_panel_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &input_panel_surface->surface_destroy_listener);

	wl_list_init(&input_panel_surface->link);

	return input_panel_surface;
}

static void
input_panel_surface_set_toplevel(struct wl_client *client,
				 struct wl_resource *resource,
				 struct wl_resource *output_resource,
				 uint32_t position)
{
	struct input_panel_surface *input_panel_surface =
		wl_resource_get_user_data(resource);
	struct desktop_shell *shell = input_panel_surface->shell;
	struct weston_head *head;

	wl_list_insert(&shell->input_panel.surfaces,
		       &input_panel_surface->link);

	head = weston_head_from_resource(output_resource);
	input_panel_surface->output = head->output;
	input_panel_surface->panel = 0;
}

static void
input_panel_surface_set_overlay_panel(struct wl_client *client,
				      struct wl_resource *resource)
{
	struct input_panel_surface *input_panel_surface =
		wl_resource_get_user_data(resource);
	struct desktop_shell *shell = input_panel_surface->shell;

	wl_list_insert(&shell->input_panel.surfaces,
		       &input_panel_surface->link);

	input_panel_surface->panel = 1;
}

static const struct weston_input_panel_surface_interface input_panel_surface_implementation = {
	input_panel_surface_set_toplevel,
	input_panel_surface_set_overlay_panel
};

static void
destroy_input_panel_surface_resource(struct wl_resource *resource)
{
	struct input_panel_surface *ipsurf =
		wl_resource_get_user_data(resource);

	destroy_input_panel_surface(ipsurf);
}

static void
input_panel_get_input_panel_surface(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id,
				    struct wl_resource *surface_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct desktop_shell *shell = wl_resource_get_user_data(resource);
	struct input_panel_surface *ipsurf;

	if (get_input_panel_surface(surface)) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "wl_input_panel::get_input_panel_surface already requested");
		return;
	}

	ipsurf = create_input_panel_surface(shell, surface);
	if (!ipsurf) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface->committed already set");
		return;
	}

	ipsurf->resource =
		wl_resource_create(client,
				   &weston_input_panel_surface_interface,
				   1,
				   id);
	wl_resource_set_implementation(ipsurf->resource,
				       &input_panel_surface_implementation,
				       ipsurf,
				       destroy_input_panel_surface_resource);
}

static const struct weston_input_panel_interface input_panel_implementation = {
	input_panel_get_input_panel_surface
};

static void
unbind_input_panel(struct wl_resource *resource)
{
	struct desktop_shell *shell = wl_resource_get_user_data(resource);

	shell->input_panel.binding = NULL;
}

static void
bind_input_panel(struct wl_client *client,
	      void *data, uint32_t version, uint32_t id)
{
	struct desktop_shell *shell = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &weston_input_panel_interface, 1, id);

	if (shell->input_panel.binding == NULL) {
		wl_resource_set_implementation(resource,
					       &input_panel_implementation,
					       shell, unbind_input_panel);
		shell->input_panel.binding = resource;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "interface object already bound");
}

void
input_panel_destroy(struct desktop_shell *shell)
{
	wl_list_remove(&shell->show_input_panel_listener.link);
	wl_list_remove(&shell->hide_input_panel_listener.link);
}

int
input_panel_setup(struct desktop_shell *shell)
{
	struct weston_compositor *ec = shell->compositor;

	shell->show_input_panel_listener.notify = show_input_panels;
	wl_signal_add(&ec->show_input_panel_signal,
		      &shell->show_input_panel_listener);
	shell->hide_input_panel_listener.notify = hide_input_panels;
	wl_signal_add(&ec->hide_input_panel_signal,
		      &shell->hide_input_panel_listener);
	shell->update_input_panel_listener.notify = update_input_panels;
	wl_signal_add(&ec->update_input_panel_signal,
		      &shell->update_input_panel_listener);

	wl_list_init(&shell->input_panel.surfaces);

	if (wl_global_create(shell->compositor->wl_display,
			     &weston_input_panel_interface, 1,
			     shell, bind_input_panel) == NULL)
		return -1;

	return 0;
}
