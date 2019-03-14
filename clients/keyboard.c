/*
 * Copyright © 2012 Openismus GmbH
 * Copyright © 2012 Intel Corporation
 * Copyright © 2019 Collabora Ltd.
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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/input.h>
#include <cairo.h>

#include "window.h"
#include "input-method-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "weston-input-panel-client-protocol.h"
#include "shared/xalloc.h"

struct keyboard;

struct text_input_state {
	uint32_t content_hint;
	uint32_t content_purpose;
	uint32_t change_cause;
	char *surrounding_text;
	uint32_t surrounding_cursor;
	bool active;
};

struct virtual_keyboard {
	struct weston_input_panel *input_panel;
	struct zwp_input_method_manager_v2 *input_method_manager;
	struct zwp_input_method_v2 *input_method;
	struct zwp_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
	struct zwp_virtual_keyboard_v1 *virtual_keyboard;
	struct display *display;
	struct output *output;
	struct wl_seat *seat;

	uint32_t serial;
	char *preedit_string;
	struct text_input_state pending;
	struct text_input_state current;
	// TODO - add support to change language (e.g. via config)
	char *preferred_language;
	struct keyboard *keyboard;
	bool toplevel;
};

enum key_type {
	keytype_default,
	keytype_backspace,
	keytype_enter,
	keytype_space,
	keytype_switch,
	keytype_symbols,
	keytype_tab,
	keytype_arrow_up,
	keytype_arrow_left,
	keytype_arrow_right,
	keytype_arrow_down,
};

struct key {
	enum key_type key_type;

	char *label;
	char *uppercase;
	char *symbol;

	unsigned int width;
};

struct layout {
	const struct key *keys;
	uint32_t count;

	uint32_t columns;
	uint32_t rows;

	const char *language;
};

static const struct key normal_keys[] = {
	{ keytype_default, "q", "Q", "1", 1},
	{ keytype_default, "w", "W", "2", 1},
	{ keytype_default, "e", "E", "3", 1},
	{ keytype_default, "r", "R", "4", 1},
	{ keytype_default, "t", "T", "5", 1},
	{ keytype_default, "y", "Y", "6", 1},
	{ keytype_default, "u", "U", "7", 1},
	{ keytype_default, "i", "I", "8", 1},
	{ keytype_default, "o", "O", "9", 1},
	{ keytype_default, "p", "P", "0", 1},
	{ keytype_backspace, "<--", "<--", "<--", 2},

	{ keytype_tab, "->|", "->|", "->|", 1},
	{ keytype_default, "a", "A", "-", 1},
	{ keytype_default, "s", "S", "@", 1},
	{ keytype_default, "d", "D", "*", 1},
	{ keytype_default, "f", "F", "^", 1},
	{ keytype_default, "g", "G", ":", 1},
	{ keytype_default, "h", "H", ";", 1},
	{ keytype_default, "j", "J", "(", 1},
	{ keytype_default, "k", "K", ")", 1},
	{ keytype_default, "l", "L", "~", 1},
	{ keytype_enter, "Enter", "Enter", "Enter", 2},

	{ keytype_switch, "ABC", "abc", "ABC", 2},
	{ keytype_default, "z", "Z", "/", 1},
	{ keytype_default, "x", "X", "\'", 1},
	{ keytype_default, "c", "C", "\"", 1},
	{ keytype_default, "v", "V", "+", 1},
	{ keytype_default, "b", "B", "=", 1},
	{ keytype_default, "n", "N", "?", 1},
	{ keytype_default, "m", "M", "!", 1},
	{ keytype_default, ",", ",", "\\", 1},
	{ keytype_default, ".", ".", "|", 1},
	{ keytype_switch, "ABC", "abc", "ABC", 1},

	{ keytype_symbols, "?123", "?123", "abc", 1},
	{ keytype_space, "", "", "", 5},
	{ keytype_arrow_up, "/\\", "/\\", "/\\", 1},
	{ keytype_arrow_left, "<", "<", "<", 1},
	{ keytype_arrow_right, ">", ">", ">", 1},
	{ keytype_arrow_down, "\\/", "\\/", "\\/", 1},
};

static const struct key numeric_keys[] = {
	{ keytype_default, "1", "1", "1", 1},
	{ keytype_default, "2", "2", "2", 1},
	{ keytype_default, "3", "3", "3", 1},
	{ keytype_default, "4", "4", "4", 1},
	{ keytype_default, "5", "5", "5", 1},
	{ keytype_default, "6", "6", "6", 1},
	{ keytype_default, "7", "7", "7", 1},
	{ keytype_default, "8", "8", "8", 1},
	{ keytype_default, "9", "9", "9", 1},
	{ keytype_default, "0", "0", "0", 1},
	{ keytype_backspace, "<--", "<--", "<--", 2},

	{ keytype_space, "", "", "", 4},
	{ keytype_enter, "Enter", "Enter", "Enter", 2},
	{ keytype_arrow_up, "/\\", "/\\", "/\\", 1},
	{ keytype_arrow_left, "<", "<", "<", 1},
	{ keytype_arrow_right, ">", ">", ">", 1},
	{ keytype_arrow_down, "\\/", "\\/", "\\/", 1},
};

static const struct key arabic_keys[] = {
	{ keytype_default, "ض", "ﹶ", "۱", 1},
	{ keytype_default, "ص", "ﹰ", "۲", 1},
	{ keytype_default, "ث", "ﹸ", "۳", 1},
	{ keytype_default, "ق", "ﹲ", "۴", 1},
	{ keytype_default, "ف", "ﻹ", "۵", 1},
	{ keytype_default, "غ", "ﺇ", "۶", 1},
	{ keytype_default, "ع", "`", "۷", 1},
	{ keytype_default, "ه", "٪", "۸", 1},
	{ keytype_default, "خ", ">", "۹", 1},
	{ keytype_default, "ح", "<", "۰", 1},
	{ keytype_backspace, "-->", "-->", "-->", 2},

	{ keytype_tab, "->|", "->|", "->|", 1},
	{ keytype_default, "ش", "ﹺ", "ﹼ", 1},
	{ keytype_default, "س", "ﹴ", "!", 1},
	{ keytype_default, "ي", "[", "@", 1},
	{ keytype_default, "ب", "]", "#", 1},
	{ keytype_default, "ل", "ﻷ", "$", 1},
	{ keytype_default, "ا", "أ", "%", 1},
	{ keytype_default, "ت", "-", "^", 1},
	{ keytype_default, "ن", "x", "&", 1},
	{ keytype_default, "م", "/", "*", 1},
	{ keytype_default, "ك", ":", "_", 1},
	{ keytype_default, "د", "\"", "+", 1},
	{ keytype_enter, "Enter", "Enter", "Enter", 2},

	{ keytype_switch, "Shift", "Base", "Shift", 2},
	{ keytype_default, "ئ", "~", ")", 1},
	{ keytype_default, "ء", "°", "(", 1},
	{ keytype_default, "ؤ", "{", "\"", 1},
	{ keytype_default, "ر", "}", "\'", 1},
	{ keytype_default, "ى", "ﺁ", "؟", 1},
	{ keytype_default, "ة", "'", "!", 1},
	{ keytype_default, "و", ",", ";", 1},
	{ keytype_default, "ﺯ", ".", "\\", 1},
	{ keytype_default, "ظ", "؟", "=", 1},
	{ keytype_switch, "Shift", "Base", "Shift", 2},

	{ keytype_symbols, "؟٣٢١", "؟٣٢١", "Base", 1},
	{ keytype_default, "ﻻ", "ﻵ", "|", 1},
	{ keytype_default, ",", "،", "،", 1},
	{ keytype_space, "", "", "", 6},
	{ keytype_default, ".", "ذ", "]", 1},
	{ keytype_default, "ط", "ﺝ", "[", 1},
};


static const struct layout normal_layout = {
	normal_keys,
	sizeof(normal_keys) / sizeof(*normal_keys),
	12,
	4,
	"en"
};

static const struct layout numeric_layout = {
	numeric_keys,
	sizeof(numeric_keys) / sizeof(*numeric_keys),
	12,
	2,
	"en"
};

static const struct layout arabic_layout = {
	arabic_keys,
	sizeof(arabic_keys) / sizeof(*arabic_keys),
	13,
	4,
	"ar"
};

static const double key_width = 60;
static const double key_height = 50;

enum keyboard_state {
	KEYBOARD_STATE_DEFAULT,
	KEYBOARD_STATE_UPPERCASE,
	KEYBOARD_STATE_SYMBOLS
};

struct keyboard {
	struct virtual_keyboard *keyboard;
	struct window *window;
	struct widget *widget;

	enum keyboard_state state;
};

static void keyboard_set_visibility(struct virtual_keyboard *virtual_keyboard,
				    bool visible);

static void __attribute__ ((format (printf, 1, 2)))
dbg(const char *fmt, ...)
{
#ifdef DEBUG
	va_list argp;

	va_start(argp, fmt);
	vfprintf(stderr, fmt, argp);
	va_end(argp);
#endif
}

static const char *
label_from_key(struct keyboard *keyboard,
	       const struct key *key)
{
	switch(keyboard->state) {
	case KEYBOARD_STATE_DEFAULT:
		return key->label;
	case KEYBOARD_STATE_UPPERCASE:
		return key->uppercase;
	case KEYBOARD_STATE_SYMBOLS:
		return key->symbol;
	}

	return "";
}

static void
draw_key(struct keyboard *keyboard,
	 const struct key *key,
	 cairo_t *cr,
	 unsigned int row,
	 unsigned int col)
{
	const char *label;
	cairo_text_extents_t extents;

	cairo_save(cr);
	cairo_rectangle(cr,
			col * key_width, row * key_height,
			key->width * key_width, key_height);
	cairo_clip(cr);

	/* Paint frame */
	cairo_rectangle(cr,
			col * key_width, row * key_height,
			key->width * key_width, key_height);
	cairo_set_line_width(cr, 3);
	cairo_stroke(cr);

	/* Paint text */
	label = label_from_key(keyboard, key);
	cairo_text_extents(cr, label, &extents);

	cairo_translate(cr,
			col * key_width,
			row * key_height);
	cairo_translate(cr,
			(key->width * key_width - extents.width) / 2,
			(key_height - extents.y_bearing) / 2);
	cairo_show_text(cr, label);

	cairo_restore(cr);
}

static const struct layout *
get_current_layout(struct virtual_keyboard *keyboard)
{
	if (!keyboard->current.active) {
		return &normal_layout;
	}

	switch (keyboard->current.content_purpose) {
		case ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DIGITS:
		case ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NUMBER:
			return &numeric_layout;
		default:
			if (keyboard->preferred_language &&
			    strcmp(keyboard->preferred_language, "ar") == 0)
				return &arabic_layout;
			else
				return &normal_layout;
	}
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct keyboard *keyboard = data;
	cairo_surface_t *surface;
	struct rectangle allocation;
	cairo_t *cr;
	unsigned int i;
	unsigned int row = 0, col = 0;
	const struct layout *layout;

	layout = get_current_layout(keyboard->keyboard);

	surface = window_get_surface(keyboard->window);
	widget_get_allocation(keyboard->widget, &allocation);

	cr = cairo_create(surface);
	cairo_rectangle(cr, allocation.x, allocation.y, allocation.width, allocation.height);
	cairo_clip(cr);

	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 16);

	cairo_translate(cr, allocation.x, allocation.y);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
	cairo_rectangle(cr, 0, 0, layout->columns * key_width, layout->rows * key_height);
	cairo_paint(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	for (i = 0; i < layout->count; ++i) {
		cairo_set_source_rgb(cr, 0, 0, 0);
		draw_key(keyboard, &layout->keys[i], cr, row, col);
		col += layout->keys[i].width;
		if (col >= layout->columns) {
			row += 1;
			col = 0;
		}
	}

	cairo_destroy(cr);
	cairo_surface_destroy(surface);
}

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	/* struct keyboard *keyboard = data; */
}

static char *
insert_text(const char *text, uint32_t offset, const char *insert)
{
	int tlen = strlen(text), ilen = strlen(insert);
	char *new_text = xmalloc(tlen + ilen + 1);

	memcpy(new_text, text, offset);
	memcpy(new_text + offset, insert, ilen);
	memcpy(new_text + offset + ilen, text + offset, tlen - offset);
	new_text[tlen + ilen] = '\0';

	return new_text;
}

static void
virtual_keyboard_commit_preedit(struct virtual_keyboard *keyboard)
{
	char *surrounding_text;

	if (!keyboard->preedit_string ||
	    strlen(keyboard->preedit_string) == 0)
		return;

	zwp_input_method_v2_commit_string(keyboard->input_method,
					  keyboard->preedit_string);
	zwp_input_method_v2_commit(keyboard->input_method,
				   keyboard->serial);

	if (keyboard->current.surrounding_text) {
		surrounding_text = insert_text(keyboard->current.surrounding_text,
					       keyboard->current.surrounding_cursor,
					       keyboard->preedit_string);
		free(keyboard->current.surrounding_text);
		keyboard->current.surrounding_text = surrounding_text;
		keyboard->current.surrounding_cursor += strlen(keyboard->preedit_string);
	} else {
		keyboard->current.surrounding_text = strdup(keyboard->preedit_string);
		keyboard->current.surrounding_cursor = strlen(keyboard->preedit_string);
	}

	free(keyboard->preedit_string);
	keyboard->preedit_string = strdup("");
}

static void
virtual_keyboard_send_preedit(struct virtual_keyboard *keyboard,
			      int32_t cursor)
{
	uint32_t index = strlen(keyboard->preedit_string);

	if (cursor > 0)
		index = cursor;
	zwp_input_method_v2_set_preedit_string(keyboard->input_method,
					       keyboard->preedit_string, index, index);
	zwp_input_method_v2_commit(keyboard->input_method, keyboard->serial);
}

static const char *
prev_utf8_char(const char *s, const char *p)
{
	for (--p; p >= s; --p) {
		if ((*p & 0xc0) != 0x80)
			return p;
	}
	return NULL;
}

static void
delete_before_cursor(struct virtual_keyboard *keyboard)
{
	const char *start, *end;
	const char *surrounding_text = keyboard->current.surrounding_text;
	uint32_t surrounding_cursor = keyboard->current.surrounding_cursor;

	if (!surrounding_text) {
		dbg("delete_before_cursor: No surrounding text available\n");
		return;
	}

	start = prev_utf8_char(surrounding_text,
			       surrounding_text + surrounding_cursor);
	if (!start) {
		dbg("delete_before_cursor: No previous character to delete\n");
		return;
	}

	end = surrounding_text + surrounding_cursor;

	zwp_input_method_v2_delete_surrounding_text(keyboard->input_method,
						    surrounding_cursor - (start - surrounding_text), 0);
	zwp_input_method_v2_commit(keyboard->input_method, keyboard->serial);

	/* Update surrounding text */
	keyboard->current.surrounding_cursor = start - surrounding_text;
	keyboard->current.surrounding_text[surrounding_cursor] = '\0';
	if (*end)
		memmove(keyboard->current.surrounding_text + keyboard->current.surrounding_cursor,
			end, strlen(end));
}

static char *
append(char *s1, const char *s2)
{
	int len1, len2;
	char *s;

	len1 = strlen(s1);
	len2 = strlen(s2);
	s = xrealloc(s1, len1 + len2 + 1);
	memcpy(s + len1, s2, len2);
	s[len1 + len2] = '\0';

	return s;
}

static void
keyboard_handle_key(struct keyboard *keyboard, uint32_t time, const struct key *key, struct input *input, enum wl_pointer_button_state state)
{
	const char *label = NULL;

	switch(keyboard->state) {
	case KEYBOARD_STATE_DEFAULT :
		label = key->label;
		break;
	case KEYBOARD_STATE_UPPERCASE :
		label = key->uppercase;
		break;
	case KEYBOARD_STATE_SYMBOLS :
		label = key->symbol;
		break;
	}

	uint32_t key_state = (state == WL_POINTER_BUTTON_STATE_PRESSED) ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;

	switch (key->key_type) {
		case keytype_default:
			if (state != WL_POINTER_BUTTON_STATE_PRESSED)
				break;

			keyboard->keyboard->preedit_string =
				append(keyboard->keyboard->preedit_string,
				       label);
			virtual_keyboard_send_preedit(keyboard->keyboard, -1);
			break;
		case keytype_backspace:
			if (!keyboard->keyboard->current.active ||
			    ((!keyboard->keyboard->preedit_string ||
			      strlen(keyboard->keyboard->preedit_string) == 0) &&
			     (!keyboard->keyboard->current.surrounding_text ||
			      strlen(keyboard->keyboard->current.surrounding_text) == 0))) {
				zwp_virtual_keyboard_v1_key(keyboard->keyboard->virtual_keyboard,
							    time, KEY_BACKSPACE, key_state);
				break;
			}

			if (state != WL_POINTER_BUTTON_STATE_PRESSED)
				break;

			if (strlen(keyboard->keyboard->preedit_string) == 0) {
				delete_before_cursor(keyboard->keyboard);
			} else {
				keyboard->keyboard->preedit_string[strlen(keyboard->keyboard->preedit_string) - 1] = '\0';
				virtual_keyboard_send_preedit(keyboard->keyboard, -1);
			}
			break;
		case keytype_enter:
			if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
			    keyboard->keyboard->current.active)
				virtual_keyboard_commit_preedit(keyboard->keyboard);
			zwp_virtual_keyboard_v1_key(keyboard->keyboard->virtual_keyboard,
						    time, KEY_ENTER, key_state);
			break;
		case keytype_space:
			if (!keyboard->keyboard->current.active) {
				zwp_virtual_keyboard_v1_key(keyboard->keyboard->virtual_keyboard,
							    time, KEY_SPACE, key_state);
				break;
			}
			if (state != WL_POINTER_BUTTON_STATE_PRESSED)
				break;
			keyboard->keyboard->preedit_string =
				append(keyboard->keyboard->preedit_string, " ");
			virtual_keyboard_commit_preedit(keyboard->keyboard);
			break;
		case keytype_switch:
			if (state != WL_POINTER_BUTTON_STATE_PRESSED)
				break;
			switch(keyboard->state) {
			case KEYBOARD_STATE_DEFAULT:
				keyboard->state = KEYBOARD_STATE_UPPERCASE;
				break;
			case KEYBOARD_STATE_UPPERCASE:
				keyboard->state = KEYBOARD_STATE_DEFAULT;
				break;
			case KEYBOARD_STATE_SYMBOLS:
				keyboard->state = KEYBOARD_STATE_UPPERCASE;
				break;
			}
			break;
		case keytype_symbols:
			if (state != WL_POINTER_BUTTON_STATE_PRESSED)
				break;
			switch(keyboard->state) {
			case KEYBOARD_STATE_DEFAULT:
				keyboard->state = KEYBOARD_STATE_SYMBOLS;
				break;
			case KEYBOARD_STATE_UPPERCASE:
				keyboard->state = KEYBOARD_STATE_SYMBOLS;
				break;
			case KEYBOARD_STATE_SYMBOLS:
				keyboard->state = KEYBOARD_STATE_DEFAULT;
				break;
			}
			break;
		case keytype_tab:
			if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
			    keyboard->keyboard->current.active)
				virtual_keyboard_commit_preedit(keyboard->keyboard);
			zwp_virtual_keyboard_v1_key(keyboard->keyboard->virtual_keyboard,
						    time, KEY_TAB, key_state);
			break;
		case keytype_arrow_up:
			if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
			    keyboard->keyboard->current.active)
				virtual_keyboard_commit_preedit(keyboard->keyboard);
			zwp_virtual_keyboard_v1_key(keyboard->keyboard->virtual_keyboard,
						    time, KEY_UP, key_state);
			break;
		case keytype_arrow_left:
			if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
			    keyboard->keyboard->current.active)
				virtual_keyboard_commit_preedit(keyboard->keyboard);
			zwp_virtual_keyboard_v1_key(keyboard->keyboard->virtual_keyboard,
						    time, KEY_LEFT, key_state);
			break;
		case keytype_arrow_right:
			if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
			    keyboard->keyboard->current.active)
				virtual_keyboard_commit_preedit(keyboard->keyboard);
			zwp_virtual_keyboard_v1_key(keyboard->keyboard->virtual_keyboard,
						    time, KEY_RIGHT, key_state);
			break;
		case keytype_arrow_down:
			if (state == WL_POINTER_BUTTON_STATE_PRESSED &&
			    keyboard->keyboard->current.active)
				virtual_keyboard_commit_preedit(keyboard->keyboard);
			zwp_virtual_keyboard_v1_key(keyboard->keyboard->virtual_keyboard,
						    time, KEY_DOWN, key_state);
			break;
	}
}

static void
button_handler(struct widget *widget,
	       struct input *input, uint32_t time,
	       uint32_t button,
	       enum wl_pointer_button_state state, void *data)
{
	struct keyboard *keyboard = data;
	struct rectangle allocation;
	int32_t x, y;
	int row, col;
	unsigned int i;
	const struct layout *layout;

	layout = get_current_layout(keyboard->keyboard);

	if (button != BTN_LEFT) {
		return;
	}

	input_get_position(input, &x, &y);

	widget_get_allocation(keyboard->widget, &allocation);
	x -= allocation.x;
	y -= allocation.y;

	row = y / key_height;
	col = x / key_width + row * layout->columns;
	for (i = 0; i < layout->count; ++i) {
		col -= layout->keys[i].width;
		if (col < 0) {
			keyboard_handle_key(keyboard, time, &layout->keys[i], input, state);
			break;
		}
	}

	widget_schedule_redraw(widget);
}

static void
touch_handler(struct input *input, uint32_t time,
	      float x, float y, uint32_t state, void *data)
{
	struct keyboard *keyboard = data;
	struct rectangle allocation;
	int row, col;
	unsigned int i;
	const struct layout *layout;

	layout = get_current_layout(keyboard->keyboard);

	widget_get_allocation(keyboard->widget, &allocation);

	x -= allocation.x;
	y -= allocation.y;

	row = (int)y / key_height;
	col = (int)x / key_width + row * layout->columns;
	for (i = 0; i < layout->count; ++i) {
		col -= layout->keys[i].width;
		if (col < 0) {
			keyboard_handle_key(keyboard, time,
					    &layout->keys[i], input, state);
			break;
		}
	}

	widget_schedule_redraw(keyboard->widget);
}

static void
touch_down_handler(struct widget *widget, struct input *input,
		   uint32_t serial, uint32_t time, int32_t id,
		   float x, float y, void *data)
{
  touch_handler(input, time, x, y,
		WL_POINTER_BUTTON_STATE_PRESSED, data);
}

static void
touch_up_handler(struct widget *widget, struct input *input,
		 uint32_t serial, uint32_t time, int32_t id,
		 void *data)
{
  float x, y;

  input_get_touch(input, id, &x, &y);

  touch_handler(input, time, x, y,
		WL_POINTER_BUTTON_STATE_RELEASED, data);
}

static void
input_method_activate(void *data,
		      struct zwp_input_method_v2 *input_method)
{
	struct virtual_keyboard *virtual_keyboard = data;
	struct text_input_state defaults = {0};

	if (virtual_keyboard->pending.surrounding_text)
		free(virtual_keyboard->pending.surrounding_text);
	virtual_keyboard->pending.surrounding_text = NULL;
	virtual_keyboard->pending = defaults;
	if (virtual_keyboard->preedit_string)
		free(virtual_keyboard->preedit_string);
	virtual_keyboard->preedit_string = strdup("");
	virtual_keyboard->pending.active = true;
}

static void
input_method_deactivate(void *data,
			struct zwp_input_method_v2 *input_method)
{
	struct virtual_keyboard *virtual_keyboard = data;

	virtual_keyboard->pending.active = false;
}

static void
handle_surrounding_text(void *data,
			struct zwp_input_method_v2 *input_method,
			const char *text,
			uint32_t cursor,
			uint32_t anchor)
{
	struct virtual_keyboard *keyboard = data;

	if (keyboard->pending.surrounding_text)
		free(keyboard->pending.surrounding_text);
	keyboard->pending.surrounding_text = strdup(text);

	keyboard->pending.surrounding_cursor = cursor;
}

static void
handle_text_change_cause(void *data,
			 struct zwp_input_method_v2 *zwp_input_method_v2,
			 uint32_t cause)
{
	struct virtual_keyboard *keyboard = data;

	keyboard->pending.change_cause = cause;
}

static void
handle_content_type(void *data,
		    struct zwp_input_method_v2 *input_method,
		    uint32_t hint,
		    uint32_t purpose)
{
	struct virtual_keyboard *keyboard = data;

	keyboard->pending.content_hint = hint;
	keyboard->pending.content_purpose = purpose;
}

static void
handle_commit_state(void *data,
		    struct zwp_input_method_v2 *input_method)
{
	struct virtual_keyboard *virtual_keyboard = data;
	struct text_input_state defaults = {0};

	virtual_keyboard->serial++;

	if (virtual_keyboard->pending.surrounding_text)
		free(virtual_keyboard->pending.surrounding_text);
	virtual_keyboard->pending.surrounding_text = NULL;
	virtual_keyboard->current = virtual_keyboard->pending;
	virtual_keyboard->pending = defaults;
	virtual_keyboard->pending.active = virtual_keyboard->current.active;

	keyboard_set_visibility(virtual_keyboard,
				virtual_keyboard->current.active);
}

static void
handle_unavailable(void *data,
		   struct zwp_input_method_v2 *zwp_input_method_v2)
{
	struct virtual_keyboard *virtual_keyboard = data;

	virtual_keyboard->current.active = false;
	zwp_input_method_v2_destroy(virtual_keyboard->input_method);
	virtual_keyboard->input_method = NULL;
	keyboard_set_visibility(virtual_keyboard, false);
}

static const struct zwp_input_method_v2_listener input_method_listener = {
	input_method_activate,
	input_method_deactivate,
	handle_surrounding_text,
	handle_text_change_cause,
	handle_content_type,
	handle_commit_state,
	handle_unavailable
};

static void
make_input_method(struct virtual_keyboard *virtual_keyboard)
{
	virtual_keyboard->input_method =
		zwp_input_method_manager_v2_get_input_method(
			virtual_keyboard->input_method_manager, virtual_keyboard->seat);
	zwp_input_method_v2_add_listener(virtual_keyboard->input_method,
		&input_method_listener, virtual_keyboard);
}

static void
make_virtual_keyboard(struct virtual_keyboard *virtual_keyboard)
{
	virtual_keyboard->virtual_keyboard =
		zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
			virtual_keyboard->virtual_keyboard_manager, virtual_keyboard->seat);
}

static void
global_handler(struct display *display, uint32_t name,
	       const char *interface, uint32_t version, void *data)
{
	struct virtual_keyboard *keyboard = data;

	if (!strcmp(interface, "weston_input_panel")) {
		keyboard->input_panel =
			display_bind(display, name,
				     &weston_input_panel_interface, 1);
	} else if (!strcmp(interface, "zwp_input_method_manager_v2")) {
		keyboard->input_method_manager =
			display_bind(display, name,
				     &zwp_input_method_manager_v2_interface, 1);
	} else if (!strcmp(interface, "zwp_virtual_keyboard_manager_v1")) {
		keyboard->virtual_keyboard_manager =
			display_bind(display, name,
				     &zwp_virtual_keyboard_manager_v1_interface, 1);
	}

}

static void
set_toplevel(struct output *output, struct virtual_keyboard *virtual_keyboard)
{
	struct weston_input_panel_surface *ips;
	struct keyboard *keyboard = virtual_keyboard->keyboard;

	ips = weston_input_panel_get_input_panel_surface(virtual_keyboard->input_panel,
							 window_get_wl_surface(keyboard->window));

	weston_input_panel_surface_set_toplevel(ips,
						output_get_wl_output(output),
						WESTON_INPUT_PANEL_SURFACE_POSITION_CENTER_BOTTOM);

	virtual_keyboard->toplevel = true;
}

static void
display_output_handler(struct output *output, void *data)
{
	struct virtual_keyboard *keyboard = data;

	if (!keyboard->toplevel && keyboard->keyboard->window)
		set_toplevel(output, keyboard);
}

static void
keyboard_window_create(struct virtual_keyboard *virtual_keyboard)
{
	struct keyboard *keyboard = virtual_keyboard->keyboard;
	const struct layout *layout;

	if (keyboard->window)
		return;

	layout = get_current_layout(virtual_keyboard);

	keyboard->window = window_create_custom(virtual_keyboard->display);
	keyboard->widget = window_add_widget(keyboard->window, keyboard);

	window_set_title(keyboard->window, "Virtual keyboard");
	window_set_user_data(keyboard->window, keyboard);

	widget_set_redraw_handler(keyboard->widget, redraw_handler);
	widget_set_resize_handler(keyboard->widget, resize_handler);
	widget_set_button_handler(keyboard->widget, button_handler);
	widget_set_touch_down_handler(keyboard->widget, touch_down_handler);
	widget_set_touch_up_handler(keyboard->widget, touch_up_handler);

	window_schedule_resize(keyboard->window,
			       layout->columns * key_width,
			       layout->rows * key_height);
}

static void
keyboard_window_destroy(struct virtual_keyboard *virtual_keyboard)
{
	if (!virtual_keyboard->keyboard->window)
		return;

	widget_destroy(virtual_keyboard->keyboard->widget);
	virtual_keyboard->keyboard->widget = NULL;
	window_destroy(virtual_keyboard->keyboard->window);
	virtual_keyboard->keyboard->window = NULL;
}

static void
keyboard_set_visibility(struct virtual_keyboard *virtual_keyboard,
			bool visible)
{
	if (visible)
		keyboard_window_create(virtual_keyboard);
	else
		keyboard_window_destroy(virtual_keyboard);
}

static void
keyboard_create(struct virtual_keyboard *virtual_keyboard)
{
	struct keyboard *keyboard;

	keyboard = xzalloc(sizeof *keyboard);
	keyboard->keyboard = virtual_keyboard;
	virtual_keyboard->keyboard = keyboard;

	display_set_output_configure_handler(virtual_keyboard->display,
					     display_output_handler);
}

int
main(int argc, char *argv[])
{
	struct virtual_keyboard virtual_keyboard;

	memset(&virtual_keyboard, 0, sizeof virtual_keyboard);

	virtual_keyboard.display = display_create(&argc, argv);
	if (virtual_keyboard.display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	display_set_user_data(virtual_keyboard.display, &virtual_keyboard);
	display_set_global_handler(virtual_keyboard.display, global_handler);

	virtual_keyboard.seat = display_get_seat(virtual_keyboard.display);

	if (virtual_keyboard.seat == NULL) {
		fprintf(stderr, "No seat available\n");
		return -1;
	}

	if (virtual_keyboard.input_method_manager == NULL) {
		fprintf(stderr, "No input method manager global\n");
		return -1;
	}

	if (virtual_keyboard.virtual_keyboard_manager == NULL) {
		fprintf(stderr, "No virtual keyboard manager global\n");
		return -1;
	}

	if (virtual_keyboard.input_panel == NULL) {
		fprintf(stderr, "No input panel global\n");
		return -1;
	}

	make_input_method(&virtual_keyboard);
	make_virtual_keyboard(&virtual_keyboard);

	keyboard_create(&virtual_keyboard);

	keyboard_set_visibility(&virtual_keyboard, true);

	display_run(virtual_keyboard.display);

	return 0;
}
