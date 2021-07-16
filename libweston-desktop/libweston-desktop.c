/*
 * Copyright © 2016 Quentin "Sardem FF7" Glidic
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

#include <string.h>

#include <wayland-server.h>
#include <assert.h>

#include <libweston/libweston.h>
#include <libweston/zalloc.h>
#include "shared/helpers.h"

#include <libweston-desktop/libweston-desktop.h>
#include "internal.h"


struct weston_desktop {
	struct weston_compositor *compositor;
	struct weston_desktop_api api;
	void *user_data;
	struct wl_global *xdg_wm_base;	 /* Stable protocol xdg_shell replaces xdg_shell_unstable_v6 */
	struct wl_global *xdg_shell_v6;  /* Unstable xdg_shell_unstable_v6 protocol. */
	struct wl_global *wl_shell;
};

void
weston_desktop_destroy_request(struct wl_client *client,
			       struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

WL_EXPORT struct weston_desktop *
weston_desktop_create(struct weston_compositor *compositor,
		      const struct weston_desktop_api *api, void *user_data)
{
	struct weston_desktop *desktop;
	struct wl_display *display = compositor->wl_display;

	assert(api->surface_added);
	assert(api->surface_removed);

	desktop = zalloc(sizeof(struct weston_desktop));
	if (!desktop)
		return NULL;

	desktop->compositor = compositor;
	desktop->user_data = user_data;

	desktop->api.struct_size =
		MIN(sizeof(struct weston_desktop_api), api->struct_size);
	memcpy(&desktop->api, api, desktop->api.struct_size);

	desktop->xdg_wm_base =
		weston_desktop_xdg_wm_base_create(desktop, display);
	if (desktop->xdg_wm_base == NULL) {
		weston_desktop_destroy(desktop);
		return NULL;
	}

	desktop->wl_shell =
		weston_desktop_wl_shell_create(desktop, display);
	if (desktop->wl_shell == NULL) {
		weston_desktop_destroy(desktop);
		return NULL;
	}

	weston_desktop_xwayland_init(desktop);

	return desktop;
}

WL_EXPORT void
weston_desktop_destroy(struct weston_desktop *desktop)
{
	if (desktop == NULL)
		return;

	weston_desktop_xwayland_fini(desktop);

	if (desktop->wl_shell != NULL)
		wl_global_destroy(desktop->wl_shell);
	if (desktop->xdg_shell_v6 != NULL)
		wl_global_destroy(desktop->xdg_shell_v6);
	if (desktop->xdg_wm_base != NULL)
		wl_global_destroy(desktop->xdg_wm_base);

	free(desktop);
}


struct weston_compositor *
weston_desktop_get_compositor(struct weston_desktop *desktop)
{
	return desktop->compositor;
}

struct wl_display *
weston_desktop_get_display(struct weston_desktop *desktop)
{
	return desktop->compositor->wl_display;
}

void
weston_desktop_api_ping_timeout(struct weston_desktop *desktop,
				struct weston_desktop_client *client)
{
	if (desktop->api.ping_timeout != NULL)
		desktop->api.ping_timeout(client, desktop->user_data);
}

void
weston_desktop_api_pong(struct weston_desktop *desktop,
			struct weston_desktop_client *client)
{
	if (desktop->api.pong != NULL)
		desktop->api.pong(client, desktop->user_data);
}

void
weston_desktop_api_surface_added(struct weston_desktop *desktop,
				 struct weston_desktop_surface *surface)
{
	struct weston_desktop_client *client =
		weston_desktop_surface_get_client(surface);
	struct wl_list *list = weston_desktop_client_get_surface_list(client);
	struct wl_list *link = weston_desktop_surface_get_client_link(surface);

	desktop->api.surface_added(surface, desktop->user_data);
	wl_list_insert(list, link);
}

void
weston_desktop_api_surface_removed(struct weston_desktop *desktop,
				   struct weston_desktop_surface *surface)
{
	struct wl_list *link = weston_desktop_surface_get_client_link(surface);

	wl_list_remove(link);
	wl_list_init(link);
	desktop->api.surface_removed(surface, desktop->user_data);
}

void
weston_desktop_api_committed(struct weston_desktop *desktop,
			     struct weston_desktop_surface *surface,
			     int32_t sx, int32_t sy)
{
	if (desktop->api.committed != NULL)
		desktop->api.committed(surface, sx, sy, desktop->user_data);
}

void
weston_desktop_api_show_window_menu(struct weston_desktop *desktop,
				    struct weston_desktop_surface *surface,
				    struct weston_seat *seat,
				    int32_t x, int32_t y)
{
	if (desktop->api.show_window_menu != NULL)
		desktop->api.show_window_menu(surface, seat, x, y,
					      desktop->user_data);
}

void
weston_desktop_api_set_parent(struct weston_desktop *desktop,
			      struct weston_desktop_surface *surface,
			      struct weston_desktop_surface *parent)
{
	if (desktop->api.set_parent != NULL)
		desktop->api.set_parent(surface, parent, desktop->user_data);
}

void
weston_desktop_api_move(struct weston_desktop *desktop,
			struct weston_desktop_surface *surface,
			struct weston_seat *seat, uint32_t serial)
{
	if (desktop->api.move != NULL)
		desktop->api.move(surface, seat, serial, desktop->user_data);
}

void
weston_desktop_api_resize(struct weston_desktop *desktop,
			  struct weston_desktop_surface *surface,
			  struct weston_seat *seat, uint32_t serial,
			  enum weston_desktop_surface_edge edges)
{
	if (desktop->api.resize != NULL)
		desktop->api.resize(surface, seat, serial, edges,
				    desktop->user_data);
}

void
weston_desktop_api_fullscreen_requested(struct weston_desktop *desktop,
					struct weston_desktop_surface *surface,
					bool fullscreen,
					struct weston_output *output)
{
	if (desktop->api.fullscreen_requested != NULL)
		desktop->api.fullscreen_requested(surface, fullscreen, output,
						  desktop->user_data);
}

void
weston_desktop_api_maximized_requested(struct weston_desktop *desktop,
				       struct weston_desktop_surface *surface,
				       bool maximized)
{
	if (desktop->api.maximized_requested != NULL)
		desktop->api.maximized_requested(surface, maximized,
						 desktop->user_data);
}

void
weston_desktop_api_minimized_requested(struct weston_desktop *desktop,
				       struct weston_desktop_surface *surface)
{
	if (desktop->api.minimized_requested != NULL)
		desktop->api.minimized_requested(surface, desktop->user_data);
}

void
weston_desktop_api_set_xwayland_position(struct weston_desktop *desktop,
					 struct weston_desktop_surface *surface,
					 int32_t x, int32_t y)
{
	if (desktop->api.set_xwayland_position != NULL)
		desktop->api.set_xwayland_position(surface, x, y,
						   desktop->user_data);
}
