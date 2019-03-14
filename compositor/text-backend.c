/*
 * Copyright © 2012 Openismus GmbH
 * Copyright © 2012 Intel Corporation
 * Copyright © 2019 Collabora Ltd.
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

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <linux/input.h>

#include "compositor.h"
#include "weston.h"
#include "input-method-unstable-v2-server-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"
#include "virtual-keyboard-unstable-v1-server-protocol.h"
#include "shared/helpers.h"
#include "shared/timespec-util.h"

struct input_method;
struct input_method_manager;
struct virtual_keyboard;
struct virtual_keyboard_manager;
struct text_backend;
struct text_input_manager;

struct text_input_state {
	struct {
		char *text; // NULL is allowed and equivalent to empty string
		uint32_t cursor;
		uint32_t anchor;
	} surrounding;

	uint32_t text_change_cause;

	struct {
		uint32_t hint;
		uint32_t purpose;
	} content_type;

	pixman_box32_t cursor_rectangle;
};

struct text_input {
	struct wl_resource *resource;

	struct weston_compositor *ec;

	struct weston_seat *seat;

	struct input_method *input_method;

	struct weston_surface *surface;

	bool input_panel_visible;

	struct text_input_manager *manager;

	uint32_t current_serial;

	bool pending_enabled;
	bool current_enabled;
	struct text_input_state pending;
	struct text_input_state current;

	struct wl_list link;
};

struct text_input_manager {
	struct wl_global *text_input_manager_global;
	struct wl_listener destroy_listener;

	struct text_input *current_text_input;

	struct weston_compositor *ec;

	struct wl_list text_inputs;
};

struct input_method_state {
	struct {
		char *text;
		int32_t cursor_begin;
		int32_t cursor_end;
	} preedit;

	char *commit_text;

	struct {
		uint32_t before_length;
		uint32_t after_length;
	} delete;
};

struct input_method {
	struct wl_resource *resource;

	struct weston_compositor *ec;

	struct weston_seat *seat;
	struct text_input *input;

	struct wl_listener keyboard_focus_listener;

	bool focus_listener_initialized;

	struct wl_resource *keyboard;

	struct input_method_manager *manager;

	struct weston_surface *pending_focused_surface;

	struct input_method_state pending;
	struct input_method_state current;

	struct wl_list link;
};

struct input_method_manager {
	struct wl_global *input_method_manager_global;
	struct wl_listener destroy_listener;

	struct weston_compositor *ec;

	struct wl_list input_methods;
};

struct virtual_keyboard {
	struct wl_resource *resource;

	struct weston_compositor *ec;

	struct weston_seat *seat;

	struct virtual_keyboard_manager *manager;

	struct wl_list link;
};

struct virtual_keyboard_manager {
	struct wl_global *virtual_keyboard_manager_global;
	struct wl_listener destroy_listener;

	struct weston_compositor *ec;

	struct wl_list virtual_keyboards;
};

struct text_backend {
	struct weston_compositor *compositor;

	struct {
		char *path;
		struct wl_client *client;

		unsigned deathcount;
		struct timespec deathstamp;
	} input_method;

	struct wl_listener client_listener;
};

static void
input_method_end_keyboard_grab(struct input_method *input_method);

static void
input_method_init_seat(struct weston_seat *seat);

static void
text_input_show_input_panel(struct text_input *text_input);

static void
deactivate_input_method(struct input_method *input_method)
{
	if (input_method->resource) {
		input_method_end_keyboard_grab(input_method);
		zwp_input_method_v2_send_deactivate(input_method->resource);
	}
	if (input_method->input) {
		input_method->input->input_method = NULL;
		input_method->input = NULL;
	}
}

static void
deactivate_text_input(struct text_input *text_input)
{
	struct weston_compositor *ec = text_input->ec;

	if (text_input->input_method &&
	    text_input->input_panel_visible &&
	    text_input->manager->current_text_input == text_input) {
		wl_signal_emit(&ec->hide_input_panel_signal, ec);
		text_input->input_panel_visible = false;
	}

	if (text_input->manager->current_text_input == text_input)
		text_input->manager->current_text_input = NULL;

	if (text_input->input_method)
		text_input->input_method->input = NULL;
	text_input->input_method = NULL;

	if (text_input->surface) {
		zwp_text_input_v3_send_leave(text_input->resource,
					     text_input->surface->resource);
	}
	text_input->surface = NULL;
}

static void
destroy_text_input(struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);

	deactivate_text_input(text_input);

	if (text_input->current.surrounding.text)
		free(text_input->current.surrounding.text);
	if (text_input->pending.surrounding.text)
		free(text_input->pending.surrounding.text);

	wl_list_remove(&text_input->link);

	free(text_input);
}

static void
text_input_set_surrounding_text(struct wl_client *client,
				struct wl_resource *resource,
				const char *text,
				int32_t cursor,
				int32_t anchor)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);

	if (text_input->pending.surrounding.text)
		free(text_input->pending.surrounding.text);
	text_input->pending.surrounding.text = strdup(text);
	text_input->pending.surrounding.cursor = cursor;
	text_input->pending.surrounding.anchor = anchor;
}

static void
activate_text_input(struct text_input *text_input)
{
	struct weston_seat *weston_seat = text_input->seat;
	struct input_method *input_method;

	if (!weston_seat)
		return;

	input_method = weston_seat->input_method;
	if (!input_method || input_method->input == text_input)
		return;

	if (!input_method->pending_focused_surface)
		return;

	input_method->input = text_input;
	text_input->input_method = input_method;

	text_input->surface = input_method->pending_focused_surface;

	zwp_input_method_v2_send_activate(input_method->resource);

	text_input->manager->current_text_input = text_input;

	text_input_show_input_panel(text_input);

	zwp_text_input_v3_send_enter(text_input->resource,
				     text_input->surface->resource);
}

static void
text_input_enable(struct wl_client *client,
		  struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct text_input_state defaults = {0};

	if (text_input->pending.surrounding.text)
		free(text_input->pending.surrounding.text);
	text_input->pending = defaults;
	text_input->pending_enabled = true;
}

static void
text_input_disable(struct wl_client *client,
		   struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);

	text_input->pending_enabled = false;
}

static void
text_input_set_cursor_rectangle(struct wl_client *client,
				struct wl_resource *resource,
				int32_t x,
				int32_t y,
				int32_t width,
				int32_t height)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);

	text_input->pending.cursor_rectangle.x1 = x;
	text_input->pending.cursor_rectangle.y1 = y;
	text_input->pending.cursor_rectangle.x2 = x + width;
	text_input->pending.cursor_rectangle.y2 = y + height;
}

static void
text_input_set_content_type(struct wl_client *client,
			    struct wl_resource *resource,
			    uint32_t hint,
			    uint32_t purpose)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);

	text_input->pending.content_type.hint = hint;
	text_input->pending.content_type.purpose = purpose;
}

static void
text_input_commit(struct wl_client *client,
		  struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct weston_compositor *ec = text_input->ec;
	struct input_method *input_method;
	bool old_enabled;

	text_input->current_serial++;
	text_input->current = text_input->pending;
	if (text_input->pending.surrounding.text)
		text_input->current.surrounding.text =
			strdup(text_input->pending.surrounding.text);

	old_enabled = text_input->current_enabled;
	text_input->current_enabled = text_input->pending_enabled;

	input_method = text_input->input_method;
	if (!old_enabled && text_input->current_enabled)
		activate_text_input(text_input);
	else if (old_enabled && !text_input->current_enabled) {
		deactivate_text_input(text_input);
	}

	if (input_method) {
		if (text_input->current.surrounding.text) {
			zwp_input_method_v2_send_surrounding_text(
						input_method->resource,
						text_input->current.surrounding.text,
						text_input->current.surrounding.cursor,
						text_input->current.surrounding.anchor);
		}
		zwp_input_method_v2_send_text_change_cause(
						input_method->resource,
						text_input->current.text_change_cause);
		zwp_input_method_v2_send_content_type(
						input_method->resource,
						text_input->current.content_type.hint,
						text_input->current.content_type.purpose);
		wl_signal_emit(&ec->update_input_panel_signal,
			       &text_input->current.cursor_rectangle);
		zwp_input_method_v2_send_done(input_method->resource);
	}
}

static void
text_input_show_input_panel(struct text_input *text_input)
{
	struct weston_compositor *ec = text_input->ec;

	text_input->input_panel_visible = true;

	if (text_input->input_method &&
	    text_input == text_input->manager->current_text_input) {
		wl_signal_emit(&ec->show_input_panel_signal,
			       text_input->surface);
		wl_signal_emit(&ec->update_input_panel_signal,
			       &text_input->current.cursor_rectangle);
	}
}

static void
text_input_destroy(struct wl_client *client,
		   struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
text_input_set_text_change_cause(struct wl_client *client,
				 struct wl_resource *resource,
				 uint32_t cause)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);

	text_input->pending.text_change_cause = cause;
}

static const struct zwp_text_input_v3_interface text_input_implementation = {
	text_input_destroy,
	text_input_enable,
	text_input_disable,
	text_input_set_surrounding_text,
	text_input_set_text_change_cause,
	text_input_set_content_type,
	text_input_set_cursor_rectangle,
	text_input_commit,
};

static void
text_input_manager_destroy(struct wl_client *client,
			   struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void text_input_manager_get_text_input(struct wl_client *client,
					      struct wl_resource *resource,
					      uint32_t id,
					      struct wl_resource *seat)
{
	struct text_input_manager *text_input_manager =
		wl_resource_get_user_data(resource);
	struct text_input *text_input;
	struct weston_seat *weston_seat = wl_resource_get_user_data(seat);

	text_input = zalloc(sizeof *text_input);
	if (text_input == NULL)
		return;

	text_input->resource =
		wl_resource_create(client, &zwp_text_input_v3_interface, 1, id);
	wl_resource_set_implementation(text_input->resource,
				       &text_input_implementation,
				       text_input, destroy_text_input);

	text_input->ec = text_input_manager->ec;
	text_input->manager = text_input_manager;
	text_input->seat = weston_seat;
	text_input->current_serial = 0;

	wl_list_insert(&text_input_manager->text_inputs, &text_input->link);
};

static const struct zwp_text_input_manager_v3_interface text_input_manager_implementation = {
	text_input_manager_destroy,
	text_input_manager_get_text_input
};

static void
bind_text_input_manager(struct wl_client *client,
			void *data,
			uint32_t version,
			uint32_t id)
{
	struct text_input_manager *text_input_manager = data;
	struct wl_resource *resource;

	/* No checking for duplicate binding necessary.  */
	resource =
		wl_resource_create(client,
				   &zwp_text_input_manager_v3_interface, 1, id);
	if (resource)
		wl_resource_set_implementation(resource,
					       &text_input_manager_implementation,
					       text_input_manager, NULL);
}

static void
text_input_manager_notifier_destroy(struct wl_listener *listener, void *data)
{
	struct text_input_manager *text_input_manager =
		container_of(listener,
			     struct text_input_manager,
			     destroy_listener);
	struct text_input *text_input, *text_input_tmp;

	wl_list_for_each_safe(text_input, text_input_tmp,
			      &text_input_manager->text_inputs, link) {
		wl_resource_destroy(text_input->resource);
	}

	wl_list_remove(&text_input_manager->destroy_listener.link);
	wl_global_destroy(text_input_manager->text_input_manager_global);

	free(text_input_manager);
}

static void
text_input_manager_create(struct weston_compositor *ec)
{
	struct text_input_manager *text_input_manager;

	text_input_manager = zalloc(sizeof *text_input_manager);
	if (text_input_manager == NULL)
		return;

	text_input_manager->ec = ec;

	text_input_manager->text_input_manager_global =
		wl_global_create(ec->wl_display,
				 &zwp_text_input_manager_v3_interface, 1,
				 text_input_manager, bind_text_input_manager);

	text_input_manager->destroy_listener.notify =
		text_input_manager_notifier_destroy;
	wl_signal_add(&ec->destroy_signal,
		      &text_input_manager->destroy_listener);

	wl_list_init(&text_input_manager->text_inputs);
}

static void
input_method_destroy(struct wl_client *client,
		     struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
input_method_commit_string(struct wl_client *client,
			   struct wl_resource *resource,
			   const char *text)
{
	struct input_method *input_method = wl_resource_get_user_data(resource);

	if (input_method->pending.commit_text)
		free(input_method->pending.commit_text);
	input_method->pending.commit_text = strdup(text);
}

static void
input_method_set_preedit_string(struct wl_client *client,
				struct wl_resource *resource,
				const char *text,
				int32_t cursor_begin,
				int32_t cursor_end)
{
	struct input_method *input_method = wl_resource_get_user_data(resource);

	if (input_method->pending.preedit.text)
		free(input_method->pending.preedit.text);
	input_method->pending.preedit.text = strdup(text);
	input_method->pending.preedit.cursor_begin = cursor_begin;
	input_method->pending.preedit.cursor_end = cursor_end;
}

static void
input_method_delete_surrounding_text(struct wl_client *client,
				     struct wl_resource *resource,
				     uint32_t before_length,
				     uint32_t after_length)
{
	struct input_method *input_method = wl_resource_get_user_data(resource);

	input_method->pending.delete.before_length = before_length;
	input_method->pending.delete.after_length = after_length;
}

static void
input_method_commit(struct wl_client *client,
		    struct wl_resource *resource,
		    uint32_t serial)
{
	struct input_method *input_method = wl_resource_get_user_data(resource);

	if (!input_method->input) {
		return;
	}

	input_method->current = input_method->pending;
	struct input_method_state default_state = {0};
	input_method->pending = default_state;

	if (input_method->current.preedit.text) {
		zwp_text_input_v3_send_preedit_string(input_method->input->resource,
						input_method->current.preedit.text,
						input_method->current.preedit.cursor_begin,
						input_method->current.preedit.cursor_end);
	}
	if (input_method->current.commit_text) {
		zwp_text_input_v3_send_commit_string(input_method->input->resource,
						input_method->current.commit_text);
	}
	if (input_method->current.delete.before_length ||
		input_method->current.delete.after_length) {
		zwp_text_input_v3_send_delete_surrounding_text(input_method->input->resource,
						input_method->current.delete.before_length,
						input_method->current.delete.after_length);
	}
	zwp_text_input_v3_send_done(input_method->input->resource,
						input_method->input->current_serial);
}

static void
unbind_keyboard(struct wl_resource *resource)
{
	struct input_method *input_method =
		wl_resource_get_user_data(resource);

	input_method_end_keyboard_grab(input_method);
	input_method->keyboard = NULL;
}

static void
input_method_context_grab_key(struct weston_keyboard_grab *grab,
			      const struct timespec *time,
			      uint32_t key,
			      uint32_t state_w)
{
	struct weston_keyboard *keyboard = grab->keyboard;
	struct wl_display *display;
	uint32_t serial;
	uint32_t msecs;

	if (!keyboard->input_method_resource)
		return;

	display = wl_client_get_display(
		wl_resource_get_client(keyboard->input_method_resource));
	serial = wl_display_next_serial(display);
	msecs = timespec_to_msec(time);
	wl_keyboard_send_key(keyboard->input_method_resource,
			     serial, msecs, key, state_w);
}

static void
input_method_context_grab_modifier(struct weston_keyboard_grab *grab,
				   uint32_t serial,
				   uint32_t mods_depressed,
				   uint32_t mods_latched,
				   uint32_t mods_locked,
				   uint32_t group)
{
	struct weston_keyboard *keyboard = grab->keyboard;

	if (!keyboard->input_method_resource)
		return;

	wl_keyboard_send_modifiers(keyboard->input_method_resource,
				   serial, mods_depressed, mods_latched,
				   mods_locked, group);
}

static void
input_method_context_grab_cancel(struct weston_keyboard_grab *grab)
{
	weston_keyboard_end_grab(grab->keyboard);
}

static const struct weston_keyboard_grab_interface input_method_context_grab = {
	input_method_context_grab_key,
	input_method_context_grab_modifier,
	input_method_context_grab_cancel,
};

static void
input_method_grab_keyboard(struct wl_client *client,
			   struct wl_resource *resource,
			   uint32_t id)
{
	struct input_method *input_method =
		wl_resource_get_user_data(resource);
	struct wl_resource *cr;
	struct weston_seat *seat = input_method->seat;
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);

	cr = wl_resource_create(client, &wl_keyboard_interface, 1, id);
	wl_resource_set_implementation(cr, NULL, input_method, unbind_keyboard);

	input_method->keyboard = cr;

	weston_keyboard_send_keymap(keyboard, cr);

	if (keyboard->grab != &keyboard->default_grab) {
		weston_keyboard_end_grab(keyboard);
	}
	weston_keyboard_start_grab(keyboard, &keyboard->input_method_grab);
	keyboard->input_method_resource = cr;
}

static void
virtual_keyboard_keymap(struct wl_client *client,
			struct wl_resource *resource,
			uint32_t format,
			int32_t fd,
			uint32_t size)
{
	weston_log("stub: zwp_virtual_keyboard_v1_interface:keymap\n");
}

static void
virtual_keyboard_key(struct wl_client *client,
		     struct wl_resource *resource,
		     uint32_t time,
		     uint32_t key,
		     uint32_t state_w)
{
	struct virtual_keyboard *virtual_keyboard =
		wl_resource_get_user_data(resource);
	struct weston_seat *seat = virtual_keyboard->seat;
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);
	struct weston_keyboard_grab *default_grab = &keyboard->default_grab;
	struct timespec ts;

	timespec_from_msec(&ts, time);

	default_grab->interface->key(default_grab, &ts, key, state_w);
}

static void
virtual_keyboard_modifiers(struct wl_client *client,
			   struct wl_resource *resource,
			   uint32_t mods_depressed,
			   uint32_t mods_latched,
			   uint32_t mods_locked,
			   uint32_t group)
{
	struct virtual_keyboard *virtual_keyboard =
		wl_resource_get_user_data(resource);

	struct weston_seat *seat = virtual_keyboard->seat;
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);
	struct weston_keyboard_grab *default_grab = &keyboard->default_grab;
	int serial = wl_display_next_serial(virtual_keyboard->ec->wl_display);

	default_grab->interface->modifiers(default_grab,
					   serial, mods_depressed,
					   mods_latched, mods_locked,
					   group);
}

static void
input_method_end_keyboard_grab(struct input_method *input_method)
{
	struct weston_keyboard_grab *grab;
	struct weston_keyboard *keyboard;

	keyboard = weston_seat_get_keyboard(input_method->seat);
	if (!keyboard)
		return;

	grab = &keyboard->input_method_grab;
	keyboard = grab->keyboard;
	if (!keyboard)
		return;

	if (keyboard->grab == grab)
		weston_keyboard_end_grab(keyboard);

	keyboard->input_method_resource = NULL;
}

static void
handle_keyboard_focus(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard = data;
	struct input_method *input_method =
		container_of(listener, struct input_method,
			     keyboard_focus_listener);
	struct weston_surface *surface = keyboard->focus;

	if (!input_method->input) {
		input_method->pending_focused_surface = surface;
		return;
	}

	if (!surface || input_method->input->surface != surface) {
		deactivate_text_input(input_method->input);
	}

	input_method->pending_focused_surface = surface;
}

static void
input_method_init_seat(struct weston_seat *seat)
{
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);

	if (seat->input_method->focus_listener_initialized)
		return;

	if (keyboard) {
		seat->input_method->keyboard_focus_listener.notify =
			handle_keyboard_focus;
		wl_signal_add(&keyboard->focus_signal,
			      &seat->input_method->keyboard_focus_listener);
		keyboard->input_method_grab.interface =
			&input_method_context_grab;
	}

	seat->input_method->focus_listener_initialized = true;
}

static void launch_input_method(struct text_backend *text_backend);

static void
respawn_input_method_process(struct text_backend *text_backend)
{
	struct timespec time;
	int64_t tdiff;

	/* if input_method dies more than 5 times in 10 seconds, give up */
	weston_compositor_get_time(&time);
	tdiff = timespec_sub_to_msec(&time,
				     &text_backend->input_method.deathstamp);
	if (tdiff > 10000) {
		text_backend->input_method.deathstamp = time;
		text_backend->input_method.deathcount = 0;
	}

	text_backend->input_method.deathcount++;
	if (text_backend->input_method.deathcount > 5) {
		weston_log("input_method disconnected, giving up.\n");
		return;
	}

	weston_log("input_method disconnected, respawning...\n");
	launch_input_method(text_backend);
}

static void
input_method_client_notifier(struct wl_listener *listener, void *data)
{
	struct text_backend *text_backend;

	text_backend = container_of(listener, struct text_backend,
				    client_listener);

	text_backend->input_method.client = NULL;
	respawn_input_method_process(text_backend);
}

static void
launch_input_method(struct text_backend *text_backend)
{
	if (!text_backend->input_method.path)
		return;

	if (strcmp(text_backend->input_method.path, "") == 0)
		return;

	text_backend->input_method.client =
		weston_client_start(text_backend->compositor,
				    text_backend->input_method.path);

	if (!text_backend->input_method.client) {
		weston_log("not able to start %s\n",
			   text_backend->input_method.path);
		return;
	}

	text_backend->client_listener.notify = input_method_client_notifier;
	wl_client_add_destroy_listener(text_backend->input_method.client,
				       &text_backend->client_listener);
}

static void input_method_get_input_popup_surface(struct wl_client *client,
						 struct wl_resource *resource,
						 uint32_t id,
						 struct wl_resource *surface)
{
	weston_log("stub: zwp_input_method_v2_interface:get_input_popup_surface\n");
}

static const struct zwp_input_method_v2_interface input_method_implementation = {
	input_method_commit_string,
	input_method_set_preedit_string,
	input_method_delete_surrounding_text,
	input_method_commit,
	input_method_get_input_popup_surface,
	input_method_grab_keyboard,
	input_method_destroy,
};

static void
destroy_input_method(struct wl_resource *resource)
{
	struct input_method *input_method =
		wl_resource_get_user_data(resource);

	if (input_method->keyboard)
		wl_resource_destroy(input_method->keyboard);

	if (input_method->input)
		deactivate_input_method(input_method);

	if (input_method->pending.commit_text)
		free(input_method->pending.commit_text);
	if (input_method->pending.preedit.text)
		free(input_method->pending.preedit.text);
	if (input_method->current.commit_text)
		free(input_method->current.commit_text);
	if (input_method->current.preedit.text)
		free(input_method->current.preedit.text);

	wl_list_remove(&input_method->link);

	free(input_method);
}

static void
input_method_manager_get_input_method(struct wl_client *client,
				      struct wl_resource *resource,
				      struct wl_resource *seat,
				      uint32_t id)
{
	struct input_method_manager *input_method_manager =
		wl_resource_get_user_data(resource);
	struct weston_seat *weston_seat = wl_resource_get_user_data(seat);
	struct input_method *input_method;

	input_method = zalloc(sizeof *input_method);
	if (input_method == NULL)
		return;

	input_method->resource =
		wl_resource_create(client, &zwp_input_method_v2_interface, 1, id);
	wl_resource_set_implementation(input_method->resource,
				       &input_method_implementation,
				       input_method, destroy_input_method);

	input_method->seat = weston_seat;
	input_method->input = NULL;
	input_method->focus_listener_initialized = false;
	input_method->manager = input_method_manager;
	input_method->pending_focused_surface = NULL;

	weston_seat->input_method = input_method;

	input_method_init_seat(weston_seat);

	wl_list_insert(&input_method_manager->input_methods, &input_method->link);
};

static void
input_method_manager_destroy(struct wl_client *client,
			     struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwp_input_method_manager_v2_interface input_method_manager_implementation = {
	input_method_manager_get_input_method,
	input_method_manager_destroy
};

static void
bind_input_method_manager(struct wl_client *client,
			  void *data,
			  uint32_t version,
			  uint32_t id)
{
	struct input_method_manager *input_method_manager = data;
	struct wl_resource *resource;

	resource =
		wl_resource_create(client,
				   &zwp_input_method_manager_v2_interface, 1, id);
	if (resource)
		wl_resource_set_implementation(resource,
					       &input_method_manager_implementation,
					       input_method_manager, NULL);
}

static void
input_method_manager_notifier_destroy(struct wl_listener *listener, void *data)
{
	struct input_method_manager *input_method_manager =
		container_of(listener,
			     struct input_method_manager,
			     destroy_listener);
	struct input_method *input_method, *input_method_tmp;

	wl_list_for_each_safe(input_method, input_method_tmp,
			      &input_method_manager->input_methods, link) {
		wl_resource_destroy(input_method->resource);
	}

	wl_list_remove(&input_method_manager->destroy_listener.link);
	wl_global_destroy(input_method_manager->input_method_manager_global);

	free(input_method_manager);
}

static void
input_method_manager_create(struct weston_compositor *ec)
{
	struct input_method_manager *input_method_manager;

	input_method_manager = zalloc(sizeof *input_method_manager);
	if (input_method_manager == NULL)
		return;

	input_method_manager->ec = ec;

	input_method_manager->input_method_manager_global =
		wl_global_create(ec->wl_display,
				 &zwp_input_method_manager_v2_interface, 1,
				 input_method_manager, bind_input_method_manager);

	input_method_manager->destroy_listener.notify =
		input_method_manager_notifier_destroy;
	wl_signal_add(&ec->destroy_signal,
		      &input_method_manager->destroy_listener);

	wl_list_init(&input_method_manager->input_methods);
}

static void
virtual_keyboard_destroy(struct wl_client *client,
			 struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwp_virtual_keyboard_v1_interface virtual_keyboard_implementation = {
	virtual_keyboard_keymap,
	virtual_keyboard_key,
	virtual_keyboard_modifiers,
	virtual_keyboard_destroy,
};

static void
destroy_virtual_keyboard(struct wl_resource *resource)
{
	struct virtual_keyboard *virtual_keyboard =
		wl_resource_get_user_data(resource);

	wl_list_remove(&virtual_keyboard->link);

	free(virtual_keyboard);
}

static void
virtual_keyboard_manager_create_virtual_keyboard(struct wl_client *client,
						 struct wl_resource *resource,
						 struct wl_resource *seat,
						 uint32_t id)
{
	struct virtual_keyboard_manager *virtual_keyboard_manager =
		wl_resource_get_user_data(resource);
	struct weston_seat *weston_seat = wl_resource_get_user_data(seat);
	struct virtual_keyboard *virtual_keyboard;

	virtual_keyboard = zalloc(sizeof *virtual_keyboard);
	if (virtual_keyboard == NULL)
		return;

	virtual_keyboard->resource =
		wl_resource_create(client, &zwp_virtual_keyboard_v1_interface, 1, id);
	wl_resource_set_implementation(virtual_keyboard->resource,
				       &virtual_keyboard_implementation,
				       virtual_keyboard, destroy_virtual_keyboard);

	virtual_keyboard->seat = weston_seat;
	virtual_keyboard->manager = virtual_keyboard_manager;

	wl_list_insert(&virtual_keyboard_manager->virtual_keyboards, &virtual_keyboard->link);
};

static void
virtual_keyboard_manager_destroy(struct wl_client *client,
				 struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zwp_virtual_keyboard_manager_v1_interface virtual_keyboard_manager_implementation = {
	virtual_keyboard_manager_create_virtual_keyboard,
	virtual_keyboard_manager_destroy,
};

static void
bind_virtual_keyboard_manager(struct wl_client *client,
			      void *data,
			      uint32_t version,
			      uint32_t id)
{
	struct virtual_keyboard_manager *virtual_keyboard_manager = data;
	struct wl_resource *resource;

	resource =
		wl_resource_create(client,
				   &zwp_virtual_keyboard_manager_v1_interface, 1, id);
	if (resource)
		wl_resource_set_implementation(resource,
					       &virtual_keyboard_manager_implementation,
					       virtual_keyboard_manager, NULL);
}

static void
virtual_keyboard_manager_notifier_destroy(struct wl_listener *listener, void *data)
{
	struct virtual_keyboard_manager *virtual_keyboard_manager =
		container_of(listener,
			     struct virtual_keyboard_manager,
			     destroy_listener);
	struct virtual_keyboard *virtual_keyboard, *virtual_keyboard_tmp;

	wl_list_for_each_safe(virtual_keyboard, virtual_keyboard_tmp,
			      &virtual_keyboard_manager->virtual_keyboards, link) {
		wl_resource_destroy(virtual_keyboard->resource);
	}

	wl_list_remove(&virtual_keyboard_manager->destroy_listener.link);
	wl_global_destroy(virtual_keyboard_manager->virtual_keyboard_manager_global);

	free(virtual_keyboard_manager);
}

static void
virtual_keyboard_manager_create(struct weston_compositor *ec)
{
	struct virtual_keyboard_manager *virtual_keyboard_manager;

	virtual_keyboard_manager = zalloc(sizeof *virtual_keyboard_manager);
	if (virtual_keyboard_manager == NULL)
		return;

	virtual_keyboard_manager->ec = ec;

	virtual_keyboard_manager->virtual_keyboard_manager_global =
		wl_global_create(ec->wl_display,
				 &zwp_virtual_keyboard_manager_v1_interface, 1,
				 virtual_keyboard_manager, bind_virtual_keyboard_manager);

	virtual_keyboard_manager->destroy_listener.notify =
		virtual_keyboard_manager_notifier_destroy;
	wl_signal_add(&ec->destroy_signal,
		      &virtual_keyboard_manager->destroy_listener);

	wl_list_init(&virtual_keyboard_manager->virtual_keyboards);
}

static void
text_backend_configuration(struct text_backend *text_backend)
{
	struct weston_config *config = wet_get_config(text_backend->compositor);
	struct weston_config_section *section;
	char *client;

	section = weston_config_get_section(config,
					    "input-method", NULL, NULL);
	client = wet_get_libexec_path("weston-keyboard");
	weston_config_section_get_string(section, "path",
					 &text_backend->input_method.path,
					 client);
	free(client);
}

WL_EXPORT void
text_backend_destroy(struct text_backend *text_backend)
{
	if (text_backend->input_method.client) {
		/* disable respawn */
		wl_list_remove(&text_backend->client_listener.link);
		wl_client_destroy(text_backend->input_method.client);
	}

	free(text_backend->input_method.path);
	free(text_backend);
}

WL_EXPORT struct text_backend *
text_backend_init(struct weston_compositor *ec)
{
	struct text_backend *text_backend;

	text_backend = zalloc(sizeof(*text_backend));
	if (text_backend == NULL)
		return NULL;

	text_backend->compositor = ec;

	text_backend_configuration(text_backend);

	input_method_manager_create(ec);
	text_input_manager_create(ec);
	virtual_keyboard_manager_create(ec);

	launch_input_method(text_backend);

	return text_backend;
}
