/*
 * Copyright © 2012 Intel Corporation
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

#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "weston-test-client-helper.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "input-method-unstable-v2-client-protocol.h"

struct text_input_state {
	int activated;
	int deactivated;
};

static void
text_input_commit_string(void *data,
			 struct zwp_text_input_v3 *text_input,
			 const char *text)
{
}

static void
text_input_preedit_string(void *data,
			  struct zwp_text_input_v3 *text_input,
			  const char *text,
			  int32_t cursor_begin,
			  int32_t cursor_end)
{
}

static void
text_input_delete_surrounding_text(void *data,
				   struct zwp_text_input_v3 *text_input,
				   uint32_t before_length,
				   uint32_t after_length)
{
}

static void
text_input_done(void *data,
		struct zwp_text_input_v3 *zwp_text_input_v3,
		uint32_t serial)
{
}

static void
text_input_enter(void *data,
		 struct zwp_text_input_v3 *text_input,
		 struct wl_surface *surface)

{
	struct text_input_state *state = data;

	fprintf(stderr, "%s\n", __FUNCTION__);

	state->activated += 1;
}

static void
text_input_leave(void *data,
		 struct zwp_text_input_v3 *text_input,
		 struct wl_surface *surface)
{
	struct text_input_state *state = data;

	state->deactivated += 1;
}

static const struct zwp_text_input_v3_listener text_input_listener = {
	text_input_enter,
	text_input_leave,
	text_input_preedit_string,
	text_input_commit_string,
	text_input_delete_surrounding_text,
	text_input_done,
};

TEST(text_test)
{
	struct client *client;
	struct global *global;
	struct zwp_input_method_manager_v2 *input_method_factory;
	struct zwp_input_method_v2 *input_method;
	struct zwp_text_input_manager_v3 *text_input_factory;
	struct zwp_text_input_v3 *text_input;
	struct text_input_state state;

	client = create_client_and_test_surface(100, 100, 100, 100);
	assert(client);

	input_method_factory = NULL;
	text_input_factory = NULL;
	wl_list_for_each(global, &client->global_list, link) {
		if (strcmp(global->interface, "zwp_input_method_manager_v2") == 0) {
			input_method_factory = wl_registry_bind(client->wl_registry,
								global->name,
								&zwp_input_method_manager_v2_interface, 1);
		}
		else if (strcmp(global->interface, "zwp_text_input_manager_v3") == 0) {
			text_input_factory = wl_registry_bind(client->wl_registry,
							      global->name,
							      &zwp_text_input_manager_v3_interface, 1);
		}
	}

	assert(input_method_factory);
	assert(text_input_factory);

	memset(&state, 0, sizeof state);

	/* Initialize input method for seat.
	 * text-input will only receive enter/leave events if there is
	 * an input method available.
	 */
	input_method = zwp_input_method_manager_v2_get_input_method(input_method_factory,
								    client->input->wl_seat);
	assert(input_method);

	/* Initialize text input for seat. */
	text_input = zwp_text_input_manager_v3_get_text_input(text_input_factory,
							      client->input->wl_seat);
	assert(text_input);
	zwp_text_input_v3_add_listener(text_input,
				       &text_input_listener,
				       &state);

	/* Make sure our test surface has keyboard focus. */
	weston_test_activate_surface(client->test->weston_test,
				 client->surface->wl_surface);
	client_roundtrip(client);
	assert(client->input->keyboard->focus == client->surface);

	/* Activate test model and make sure we get enter event. */
	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_commit(text_input);
	client_roundtrip(client);
	assert(state.activated == 1 && state.deactivated == 0);

	/* Deactivate test model and make sure we get leave event. */
	zwp_text_input_v3_disable(text_input);
	zwp_text_input_v3_commit(text_input);
	client_roundtrip(client);
	assert(state.activated == 1 && state.deactivated == 1);

	/* Activate test model again. */
	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_commit(text_input);
	client_roundtrip(client);
	assert(state.activated == 2 && state.deactivated == 1);

	/* Take keyboard focus away and verify we get leave event. */
	weston_test_activate_surface(client->test->weston_test, NULL);
	client_roundtrip(client);
	assert(state.activated == 2 && state.deactivated == 2);
}
