/*
 * Copyright © 2012 Philipp Brüschweiler
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

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <ctype.h>

#include <wayland-client.h>

#include "shared/helpers.h"
#include "shared/os-compatibility.h"
#include "shared/xalloc.h"
#include "shared/zalloc.h"
#include "presentation-time-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"

typedef void (*print_info_t)(void *info);
typedef void (*destroy_info_t)(void *info);

struct global_info {
	struct wl_list link;

	uint32_t id;
	uint32_t version;
	char *interface;

	print_info_t print;
	destroy_info_t destroy;
};

struct output_mode {
	struct wl_list link;

	uint32_t flags;
	int32_t width, height;
	int32_t refresh;
};

struct output_info {
	struct global_info global;

	struct wl_output *output;

	int32_t version;

	struct {
		int32_t x, y;
		int32_t scale;
		int32_t physical_width, physical_height;
		enum wl_output_subpixel subpixel;
		enum wl_output_transform output_transform;
		char *make;
		char *model;
	} geometry;

	struct wl_list modes;
};

struct shm_format {
	struct wl_list link;

	uint32_t format;
};

struct shm_info {
	struct global_info global;
	struct wl_shm *shm;

	struct wl_list formats;
};

struct linux_dmabuf_modifier {
	struct wl_list link;

	uint32_t format;
	uint64_t modifier;
};

struct linux_dmabuf_info {
	struct global_info global;
	struct zwp_linux_dmabuf_v1 *dmabuf;

	struct wl_list modifiers;
};

struct seat_info {
	struct global_info global;
	struct wl_list global_link;
	struct wl_seat *seat;
	struct weston_info *info;

	struct wl_keyboard *keyboard;
	uint32_t capabilities;
	char *name;

	int32_t repeat_rate;
	int32_t repeat_delay;
};

struct tablet_v2_path {
	struct wl_list link;
	char *path;
};

struct tablet_tool_info {
	struct wl_list link;
	struct zwp_tablet_tool_v2 *tool;

	uint64_t hardware_serial;
	uint64_t hardware_id_wacom;
	enum zwp_tablet_tool_v2_type type;
	
	bool has_tilt;
	bool has_pressure;
	bool has_distance;
	bool has_rotation;
	bool has_slider;
	bool has_wheel;
};

struct tablet_pad_group_info {
	struct wl_list link;
	struct zwp_tablet_pad_group_v2 *group;

	uint32_t modes;
	size_t button_count;
	int *buttons;
	size_t strips;
	size_t rings;
};

struct tablet_pad_info {
	struct wl_list link;
	struct zwp_tablet_pad_v2 *pad;

	uint32_t buttons;
	struct wl_list paths;
	struct wl_list groups;
};

struct tablet_info {
	struct wl_list link;
	struct zwp_tablet_v2 *tablet;

	char *name;
	uint32_t vid, pid;
	struct wl_list paths;
};

struct tablet_seat_info {
	struct wl_list link;

	struct zwp_tablet_seat_v2 *seat;
	struct seat_info *seat_info;

	struct wl_list tablets;
	struct wl_list tools;
	struct wl_list pads;
};

struct tablet_v2_info {
	struct global_info global;
	struct zwp_tablet_manager_v2 *manager;
	struct weston_info *info;

	struct wl_list seats;
};

struct presentation_info {
	struct global_info global;
	struct wp_presentation *presentation;

	clockid_t clk_id;
};

struct weston_info {
	struct wl_display *display;
	struct wl_registry *registry;

	struct wl_list infos;
	bool roundtrip_needed;

	/* required for tablet-unstable-v2 */
	struct wl_list seats;
	struct tablet_v2_info *tablet_info;
};

static void
print_global_info(void *data)
{
	struct global_info *global = data;

	printf("interface: '%s', version: %u, name: %u\n",
	       global->interface, global->version, global->id);
}

static void
init_global_info(struct weston_info *info,
		 struct global_info *global, uint32_t id,
		 const char *interface, uint32_t version)
{
	global->id = id;
	global->version = version;
	global->interface = xstrdup(interface);

	wl_list_insert(info->infos.prev, &global->link);
}

static void
print_output_info(void *data)
{
	struct output_info *output = data;
	struct output_mode *mode;
	const char *subpixel_orientation;
	const char *transform;

	print_global_info(data);

	switch (output->geometry.subpixel) {
	case WL_OUTPUT_SUBPIXEL_UNKNOWN:
		subpixel_orientation = "unknown";
		break;
	case WL_OUTPUT_SUBPIXEL_NONE:
		subpixel_orientation = "none";
		break;
	case WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB:
		subpixel_orientation = "horizontal rgb";
		break;
	case WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR:
		subpixel_orientation = "horizontal bgr";
		break;
	case WL_OUTPUT_SUBPIXEL_VERTICAL_RGB:
		subpixel_orientation = "vertical rgb";
		break;
	case WL_OUTPUT_SUBPIXEL_VERTICAL_BGR:
		subpixel_orientation = "vertical bgr";
		break;
	default:
		fprintf(stderr, "unknown subpixel orientation %u\n",
			output->geometry.subpixel);
		subpixel_orientation = "unexpected value";
		break;
	}

	switch (output->geometry.output_transform) {
	case WL_OUTPUT_TRANSFORM_NORMAL:
		transform = "normal";
		break;
	case WL_OUTPUT_TRANSFORM_90:
		transform = "90°";
		break;
	case WL_OUTPUT_TRANSFORM_180:
		transform = "180°";
		break;
	case WL_OUTPUT_TRANSFORM_270:
		transform = "270°";
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		transform = "flipped";
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		transform = "flipped 90°";
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		transform = "flipped 180°";
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		transform = "flipped 270°";
		break;
	default:
		fprintf(stderr, "unknown output transform %u\n",
			output->geometry.output_transform);
		transform = "unexpected value";
		break;
	}

	printf("\tx: %d, y: %d,",
	       output->geometry.x, output->geometry.y);
	if (output->version >= 2)
		printf(" scale: %d,", output->geometry.scale);
	printf("\n");

	printf("\tphysical_width: %d mm, physical_height: %d mm,\n",
	       output->geometry.physical_width,
	       output->geometry.physical_height);
	printf("\tmake: '%s', model: '%s',\n",
	       output->geometry.make, output->geometry.model);
	printf("\tsubpixel_orientation: %s, output_transform: %s,\n",
	       subpixel_orientation, transform);

	wl_list_for_each(mode, &output->modes, link) {
		printf("\tmode:\n");

		printf("\t\twidth: %d px, height: %d px, refresh: %.3f Hz,\n",
		       mode->width, mode->height,
		       (float) mode->refresh / 1000);

		printf("\t\tflags:");
		if (mode->flags & WL_OUTPUT_MODE_CURRENT)
			printf(" current");
		if (mode->flags & WL_OUTPUT_MODE_PREFERRED)
			printf(" preferred");
		printf("\n");
	}
}

static char
bits2graph(uint32_t value, unsigned bitoffset)
{
	int c = (value >> bitoffset) & 0xff;

	if (isgraph(c) || isspace(c))
		return c;

	return '?';
}

static void
fourcc2str(uint32_t format, char *str, int len)
{
	int i;

	assert(len >= 5);

	for (i = 0; i < 4; i++)
		str[i] = bits2graph(format, i * 8);
	str[i] = '\0';
}

static void
print_shm_info(void *data)
{
	char str[5];
	struct shm_info *shm = data;
	struct shm_format *format;

	print_global_info(data);

	printf("\tformats:");

	wl_list_for_each(format, &shm->formats, link)
		switch (format->format) {
		case WL_SHM_FORMAT_ARGB8888:
			printf(" ARGB8888");
			break;
		case WL_SHM_FORMAT_XRGB8888:
			printf(" XRGB8888");
			break;
		case WL_SHM_FORMAT_RGB565:
			printf(" RGB565");
			break;
		default:
			fourcc2str(format->format, str, sizeof(str));
			printf(" '%s'(0x%08x)", str, format->format);
			break;
		}

	printf("\n");
}

static void
print_linux_dmabuf_info(void *data)
{
	char str[5];
	struct linux_dmabuf_info *dmabuf = data;
	struct linux_dmabuf_modifier *modifier;

	print_global_info(data);

	printf("\tformats:");

	wl_list_for_each(modifier, &dmabuf->modifiers, link) {
		fourcc2str(modifier->format, str, sizeof(str));
		printf("\n\t'%s'(0x%08x), modifier: 0x%016"PRIx64, str, modifier->format, modifier->modifier);
	}

	printf("\n");
}

static void
print_seat_info(void *data)
{
	struct seat_info *seat = data;

	print_global_info(data);

	printf("\tname: %s\n", seat->name);
	printf("\tcapabilities:");

	if (seat->capabilities & WL_SEAT_CAPABILITY_POINTER)
		printf(" pointer");
	if (seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD)
		printf(" keyboard");
	if (seat->capabilities & WL_SEAT_CAPABILITY_TOUCH)
		printf(" touch");

	printf("\n");

	if (seat->repeat_rate > 0)
		printf("\tkeyboard repeat rate: %d\n", seat->repeat_rate);
	if (seat->repeat_delay > 0)
		printf("\tkeyboard repeat delay: %d\n", seat->repeat_delay);
}

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
		       uint32_t format, int fd, uint32_t size)
{
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
		      uint32_t serial, struct wl_surface *surface,
		      struct wl_array *keys)
{
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
		      uint32_t serial, struct wl_surface *surface)
{
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
		    uint32_t serial, uint32_t time, uint32_t key,
		    uint32_t state)
{
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
			  uint32_t serial, uint32_t mods_depressed,
			  uint32_t mods_latched, uint32_t mods_locked,
			  uint32_t group)
{
}

static void
keyboard_handle_repeat_info(void *data, struct wl_keyboard *keyboard,
			    int32_t rate, int32_t delay)
{
	struct seat_info *seat = data;

	seat->repeat_rate = rate;
	seat->repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_handle_keymap,
	keyboard_handle_enter,
	keyboard_handle_leave,
	keyboard_handle_key,
	keyboard_handle_modifiers,
	keyboard_handle_repeat_info,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
			 enum wl_seat_capability caps)
{
	struct seat_info *seat = data;

	seat->capabilities = caps;

	/* we want listen for repeat_info from wl_keyboard, but only
	 * do so if the seat info is >= 4 and if we actually have a
	 * keyboard */
	if (seat->global.version < 4)
		return;

	if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
		seat->keyboard = wl_seat_get_keyboard(seat->seat);
		wl_keyboard_add_listener(seat->keyboard, &keyboard_listener,
					 seat);

		seat->info->roundtrip_needed = true;
	}
}

static void
seat_handle_name(void *data, struct wl_seat *wl_seat,
		 const char *name)
{
	struct seat_info *seat = data;
	seat->name = xstrdup(name);
}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
	seat_handle_name,
};

static void
destroy_seat_info(void *data)
{
	struct seat_info *seat = data;

	wl_seat_destroy(seat->seat);

	if (seat->name != NULL)
		free(seat->name);

	if (seat->keyboard)
		wl_keyboard_destroy(seat->keyboard);

	wl_list_remove(&seat->global_link);
}

static const char *
tablet_tool_type_to_str(enum zwp_tablet_tool_v2_type type)
{
	switch (type) {
	case ZWP_TABLET_TOOL_V2_TYPE_PEN:
		return "pen";
	case ZWP_TABLET_TOOL_V2_TYPE_ERASER:
		return "eraser";
	case ZWP_TABLET_TOOL_V2_TYPE_BRUSH:
		return "brush";
	case ZWP_TABLET_TOOL_V2_TYPE_PENCIL:
		return "pencil";
	case ZWP_TABLET_TOOL_V2_TYPE_AIRBRUSH:
		return "airbrush";
	case ZWP_TABLET_TOOL_V2_TYPE_FINGER:
		return "finger";
	case ZWP_TABLET_TOOL_V2_TYPE_MOUSE:
		return "mouse";
	case ZWP_TABLET_TOOL_V2_TYPE_LENS:
		return "lens";
	}

	return "Unknown type";
}

static void
print_tablet_tool_info(const struct tablet_tool_info *info)
{
	printf("\t\ttablet_tool: %s\n", tablet_tool_type_to_str(info->type));
	if (info->hardware_serial) {
		printf("\t\t\thardware serial: %lx\n", info->hardware_serial);
	}
	if (info->hardware_id_wacom) {
		printf("\t\t\thardware wacom: %lx\n", info->hardware_id_wacom);
	}

	printf("\t\t\tcapabilities:");

	if (info->has_tilt) {
		printf(" tilt");
	}
	if (info->has_pressure) {
		printf(" pressure");
	}
	if (info->has_distance) {
		printf(" distance");
	}
	if (info->has_rotation) {
		printf(" rotation");
	}
	if (info->has_slider) {
		printf(" slider");
	}
	if (info->has_wheel) {
		printf(" wheel");
	}
	printf("\n");
}

static void
destroy_tablet_tool_info(struct tablet_tool_info *info)
{
	wl_list_remove(&info->link);
	zwp_tablet_tool_v2_destroy(info->tool);
	free(info);
}

static void
print_tablet_pad_group_info(const struct tablet_pad_group_info *info)
{
	size_t i;
	printf("\t\t\tgroup:\n");
	printf("\t\t\t\tmodes: %u\n", info->modes);
	printf("\t\t\t\tstrips: %zu\n", info->strips);
	printf("\t\t\t\trings: %zu\n", info->rings);
	printf("\t\t\t\tbuttons:");

	for (i = 0; i < info->button_count; ++i) {
		printf(" %d", info->buttons[i]);
	}

	printf("\n");
}

static void
destroy_tablet_pad_group_info(struct tablet_pad_group_info *info)
{
	wl_list_remove(&info->link);
	zwp_tablet_pad_group_v2_destroy(info->group);

	if (info->buttons) {
		free(info->buttons);
	}
	free(info);
}

static void
print_tablet_pad_info(const struct tablet_pad_info *info)
{
	const struct tablet_v2_path *path;
	const struct tablet_pad_group_info *group;

	printf("\t\tpad:\n");
	printf("\t\t\tbuttons: %u\n", info->buttons);

	wl_list_for_each(path, &info->paths, link) {
		printf("\t\t\tpath: %s\n", path->path);
	}

	wl_list_for_each(group, &info->groups, link) {
		print_tablet_pad_group_info(group);
	}
}

static void
destroy_tablet_pad_info(struct tablet_pad_info *info)
{
	struct tablet_v2_path *path;
	struct tablet_v2_path *tmp_path;
	struct tablet_pad_group_info *group;
	struct tablet_pad_group_info *tmp_group;

	wl_list_remove(&info->link);
	zwp_tablet_pad_v2_destroy(info->pad);

	wl_list_for_each_safe(path, tmp_path, &info->paths, link) {
		wl_list_remove(&path->link);
		free(path->path);
		free(path);
	}

	wl_list_for_each_safe(group, tmp_group, &info->groups, link) {
		destroy_tablet_pad_group_info(group);
	}

	free(info);
}

static void
print_tablet_info(const struct tablet_info *info)
{
	const struct tablet_v2_path *path;

	printf("\t\ttablet: %s\n", info->name);
	printf("\t\t\tvendor: %u\n", info->vid);
	printf("\t\t\tproduct: %u\n", info->pid);

	wl_list_for_each(path, &info->paths, link) {
		printf("\t\t\tpath: %s\n", path->path);
	}
}

static void
destroy_tablet_info(struct tablet_info *info)
{
	struct tablet_v2_path *path;
	struct tablet_v2_path *tmp;

	wl_list_remove(&info->link);
	zwp_tablet_v2_destroy(info->tablet);

	if (info->name) {
		free(info->name);
	}

	wl_list_for_each_safe(path, tmp, &info->paths, link) {
		wl_list_remove(&path->link);
		free(path->path);
		free(path);
	}

	free(info);
}

static void
print_tablet_seat_info(const struct tablet_seat_info *info)
{
	const struct tablet_info *tablet;
	const struct tablet_pad_info *pad;
	const struct tablet_tool_info *tool;

	printf("\ttablet_seat: %s\n", info->seat_info->name);

	wl_list_for_each(tablet, &info->tablets, link) {
		print_tablet_info(tablet);
	}

	wl_list_for_each(pad, &info->pads, link) {
		print_tablet_pad_info(pad);
	}

	wl_list_for_each(tool, &info->tools, link) {
		print_tablet_tool_info(tool);
	}
}

static void
destroy_tablet_seat_info(struct tablet_seat_info *info)
{
	struct tablet_info *tablet;
	struct tablet_info *tmp_tablet;
	struct tablet_pad_info *pad;
	struct tablet_pad_info *tmp_pad;
	struct tablet_tool_info *tool;
	struct tablet_tool_info *tmp_tool;

	wl_list_remove(&info->link);
	zwp_tablet_seat_v2_destroy(info->seat);

	wl_list_for_each_safe(tablet, tmp_tablet, &info->tablets, link) {
		destroy_tablet_info(tablet);
	}

	wl_list_for_each_safe(pad, tmp_pad, &info->pads, link) {
		destroy_tablet_pad_info(pad);
	}

	wl_list_for_each_safe(tool, tmp_tool, &info->tools, link) {
		destroy_tablet_tool_info(tool);
	}

	free(info);
}

static void
print_tablet_v2_info(void *data)
{
	struct tablet_v2_info *info = data;
	struct tablet_seat_info *seat;
	print_global_info(data);

	wl_list_for_each(seat, &info->seats, link) {
		/* Skip tablet_seats without a tablet, they are irrelevant */
		if (wl_list_empty(&seat->pads) &&
		    wl_list_empty(&seat->tablets) &&
		    wl_list_empty(&seat->tools)) {
			continue;
		}

		print_tablet_seat_info(seat);
	}
}

static void
destroy_tablet_v2_info(void *data)
{
	struct tablet_v2_info *info = data;
	struct tablet_seat_info *seat;
	struct tablet_seat_info *tmp;

	zwp_tablet_manager_v2_destroy(info->manager);

	wl_list_for_each_safe(seat, tmp, &info->seats, link) {
		destroy_tablet_seat_info(seat);
	}
}

static void
handle_tablet_v2_tablet_tool_done(void *data, struct zwp_tablet_tool_v2 *tool)
{
	/* don't bother waiting for this; there's no good reason a
	 * compositor will wait more than one roundtrip before sending
	 * these initial events. */
}

static void
handle_tablet_v2_tablet_tool_removed(void *data, struct zwp_tablet_tool_v2 *tool)
{
	/* don't bother waiting for this; we never make any request either way. */
}

static void
handle_tablet_v2_tablet_tool_type(void *data, struct zwp_tablet_tool_v2 *tool,
                                  uint32_t tool_type)
{
	struct tablet_tool_info *info = data;
	info->type = tool_type;
}

static void
handle_tablet_v2_tablet_tool_hardware_serial(void *data,
                                             struct zwp_tablet_tool_v2 *tool,
                                             uint32_t serial_hi,
                                             uint32_t serial_lo)
{
	struct tablet_tool_info *info = data;

	info->hardware_serial = ((uint64_t) serial_hi) << 32 |
		(uint64_t) serial_lo;
}

static void
handle_tablet_v2_tablet_tool_hardware_id_wacom(void *data,
                                               struct zwp_tablet_tool_v2 *tool,
                                               uint32_t id_hi, uint32_t id_lo)
{
	struct tablet_tool_info *info = data;

	info->hardware_id_wacom = ((uint64_t) id_hi) << 32 | (uint64_t) id_lo;
}

static void
handle_tablet_v2_tablet_tool_capability(void *data,
                                        struct zwp_tablet_tool_v2 *tool,
                                        uint32_t capability)
{
	struct tablet_tool_info *info = data;
	enum zwp_tablet_tool_v2_capability cap = capability;
	
	switch(cap) {
	case ZWP_TABLET_TOOL_V2_CAPABILITY_TILT:
		info->has_tilt = true;
		break;
	case ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE:
		info->has_pressure = true;
		break;
	case ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE:
		info->has_distance = true;
		break;
	case ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION:
		info->has_rotation = true;
		break;
	case ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER:
		info->has_slider = true;
		break;
	case ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL:
		info->has_wheel = true;
		break;
	}
}

static void
handle_tablet_v2_tablet_tool_proximity_in(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 uint32_t serial, struct zwp_tablet_v2 *tablet,
                                 struct wl_surface *surface)
{

}

static void
handle_tablet_v2_tablet_tool_proximity_out(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2)
{

}

static void
handle_tablet_v2_tablet_tool_down(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 uint32_t serial)
{

}

static void
handle_tablet_v2_tablet_tool_up(void *data,
                                struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2)
{

}


static void
handle_tablet_v2_tablet_tool_motion(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 wl_fixed_t x,
                                 wl_fixed_t y)
{

}

static void
handle_tablet_v2_tablet_tool_pressure(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 uint32_t pressure)
{

}

static void
handle_tablet_v2_tablet_tool_distance(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 uint32_t distance)
{

}

static void
handle_tablet_v2_tablet_tool_tilt(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 wl_fixed_t tilt_x,
                                 wl_fixed_t tilt_y)
{

}

static void
handle_tablet_v2_tablet_tool_rotation(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 wl_fixed_t degrees)
{

}

static void
handle_tablet_v2_tablet_tool_slider(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 int32_t position)
{

}

static void
handle_tablet_v2_tablet_tool_wheel(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 wl_fixed_t degrees,
                                 int32_t clicks)
{

}

static void
handle_tablet_v2_tablet_tool_button(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 uint32_t serial,
                                 uint32_t button,
                                 uint32_t state)
{

}

static void
handle_tablet_v2_tablet_tool_frame(void *data,
                                 struct zwp_tablet_tool_v2 *zwp_tablet_tool_v2,
                                 uint32_t time)
{

}

static const struct zwp_tablet_tool_v2_listener tablet_tool_listener = {
	.removed = handle_tablet_v2_tablet_tool_removed,
	.done = handle_tablet_v2_tablet_tool_done,
	.type = handle_tablet_v2_tablet_tool_type,
	.hardware_serial = handle_tablet_v2_tablet_tool_hardware_serial,
	.hardware_id_wacom = handle_tablet_v2_tablet_tool_hardware_id_wacom,
	.capability = handle_tablet_v2_tablet_tool_capability,

	.proximity_in = handle_tablet_v2_tablet_tool_proximity_in,
	.proximity_out = handle_tablet_v2_tablet_tool_proximity_out,
	.down = handle_tablet_v2_tablet_tool_down,
	.up = handle_tablet_v2_tablet_tool_up,

	.motion = handle_tablet_v2_tablet_tool_motion,
	.pressure = handle_tablet_v2_tablet_tool_pressure,
	.distance = handle_tablet_v2_tablet_tool_distance,
	.tilt = handle_tablet_v2_tablet_tool_tilt,
	.rotation = handle_tablet_v2_tablet_tool_rotation,
	.slider = handle_tablet_v2_tablet_tool_slider,
	.wheel = handle_tablet_v2_tablet_tool_wheel,
	.button = handle_tablet_v2_tablet_tool_button,
	.frame = handle_tablet_v2_tablet_tool_frame,
};

static void add_tablet_v2_tablet_tool_info(void *data,
                                     struct zwp_tablet_seat_v2 *tablet_seat_v2,
                                     struct zwp_tablet_tool_v2 *tool)
{
	struct tablet_seat_info *tablet_seat = data;
	struct tablet_tool_info *tool_info = xzalloc(sizeof *tool_info);

	tool_info->tool = tool;
	wl_list_insert(&tablet_seat->tools, &tool_info->link);

	zwp_tablet_tool_v2_add_listener(tool, &tablet_tool_listener, tool_info);
}

static void
handle_tablet_v2_tablet_pad_group_mode_switch(void *data,
                       struct zwp_tablet_pad_group_v2 *zwp_tablet_pad_group_v2,
                       uint32_t time, uint32_t serial, uint32_t mode)
{
	/* This shouldn't ever happen  */
}

static void
handle_tablet_v2_tablet_pad_group_done(void *data,
                                       struct zwp_tablet_pad_group_v2 *group)
{
	/* don't bother waiting for this; there's no good reason a
	 * compositor will wait more than one roundtrip before sending
	 * these initial events. */
}

static void
handle_tablet_v2_tablet_pad_group_modes(void *data,
                                        struct zwp_tablet_pad_group_v2 *group,
                                        uint32_t modes)
{
	struct tablet_pad_group_info *info = data;
	info->modes = modes;
}

static void
handle_tablet_v2_tablet_pad_group_buttons(void *data,
                                          struct zwp_tablet_pad_group_v2 *group,
                                          struct wl_array *buttons)
{
	struct tablet_pad_group_info *info = data;

	info->button_count = buttons->size / sizeof(int);
	info->buttons = xzalloc(buttons->size);
	memcpy(info->buttons, buttons->data, buttons->size);
}

static void
handle_tablet_v2_tablet_pad_group_ring(void *data,
                                       struct zwp_tablet_pad_group_v2 *group,
                                       struct zwp_tablet_pad_ring_v2 *ring)
{
	struct tablet_pad_group_info *info = data;
	++info->rings;

	zwp_tablet_pad_ring_v2_destroy(ring);
}

static void
handle_tablet_v2_tablet_pad_group_strip(void *data,
                                        struct zwp_tablet_pad_group_v2 *group,
                                        struct zwp_tablet_pad_strip_v2 *strip)
{
	struct tablet_pad_group_info *info = data;
	++info->strips;

	zwp_tablet_pad_strip_v2_destroy(strip);
}

static const struct zwp_tablet_pad_group_v2_listener tablet_pad_group_listener = {
	.buttons = handle_tablet_v2_tablet_pad_group_buttons,
	.modes = handle_tablet_v2_tablet_pad_group_modes,
	.ring = handle_tablet_v2_tablet_pad_group_ring,
	.strip = handle_tablet_v2_tablet_pad_group_strip,
	.done = handle_tablet_v2_tablet_pad_group_done,
	.mode_switch = handle_tablet_v2_tablet_pad_group_mode_switch,
};

static void
handle_tablet_v2_tablet_pad_group(void *data,
                                  struct zwp_tablet_pad_v2 *zwp_tablet_pad_v2,
                                  struct zwp_tablet_pad_group_v2 *pad_group)
{
	struct tablet_pad_info *pad_info = data;
	struct tablet_pad_group_info *group = xzalloc(sizeof *group);

	wl_list_insert(&pad_info->groups, &group->link);
	group->group = pad_group;
	zwp_tablet_pad_group_v2_add_listener(pad_group,
	                                     &tablet_pad_group_listener, group);
}

static void
handle_tablet_v2_tablet_pad_path(void *data, struct zwp_tablet_pad_v2 *pad,
                                 const char *path)
{
	struct tablet_pad_info *pad_info = data;
	struct tablet_v2_path *path_elem = xzalloc(sizeof *path_elem);
	path_elem->path = xstrdup(path);

	wl_list_insert(&pad_info->paths, &path_elem->link);
}

static void
handle_tablet_v2_tablet_pad_buttons(void *data, struct zwp_tablet_pad_v2 *pad,
                                    uint32_t buttons)
{
	struct tablet_pad_info *pad_info = data;

	pad_info->buttons = buttons;
}

static void
handle_tablet_v2_tablet_pad_done(void *data, struct zwp_tablet_pad_v2 *pad)
{
	/* don't bother waiting for this; there's no good reason a
	 * compositor will wait more than one roundtrip before sending
	 * these initial events. */
}

static void
handle_tablet_v2_tablet_pad_removed(void *data, struct zwp_tablet_pad_v2 *pad)
{
	/* don't bother waiting for this; We never make any request that's not
	 * allowed to be issued either way. */
}

static void
handle_tablet_v2_tablet_pad_button(void *data, struct zwp_tablet_pad_v2 *pad,
                                   uint32_t time, uint32_t button, uint32_t state)
{
	/* we don't have a surface, so this can't ever happen */
}

static void
handle_tablet_v2_tablet_pad_enter(void *data, struct zwp_tablet_pad_v2 *pad,
                                  uint32_t serial,
                                  struct zwp_tablet_v2 *tablet,
                                  struct wl_surface *surface)
{
	/* we don't have a surface, so this can't ever happen */
}

static void
handle_tablet_v2_tablet_pad_leave(void *data, struct zwp_tablet_pad_v2 *pad,
		uint32_t serial, struct wl_surface *surface)
{
	/* we don't have a surface, so this can't ever happen */
}

static const struct zwp_tablet_pad_v2_listener tablet_pad_listener = {
	.group = handle_tablet_v2_tablet_pad_group,
	.path = handle_tablet_v2_tablet_pad_path,
	.buttons = handle_tablet_v2_tablet_pad_buttons,
	.done = handle_tablet_v2_tablet_pad_done,
	.removed = handle_tablet_v2_tablet_pad_removed,
	.button = handle_tablet_v2_tablet_pad_button,
	.enter = handle_tablet_v2_tablet_pad_enter,
	.leave = handle_tablet_v2_tablet_pad_leave,
};

static void add_tablet_v2_tablet_pad_info(void *data,
                                     struct zwp_tablet_seat_v2 *tablet_seat_v2,
                                     struct zwp_tablet_pad_v2 *pad)
{
	struct tablet_seat_info *tablet_seat = data;
	struct tablet_pad_info *pad_info = xzalloc(sizeof *pad_info);

	wl_list_init(&pad_info->paths);
	wl_list_init(&pad_info->groups);
	pad_info->pad = pad;
	wl_list_insert(&tablet_seat->pads, &pad_info->link);

	zwp_tablet_pad_v2_add_listener(pad, &tablet_pad_listener, pad_info);
}

static void
handle_tablet_v2_tablet_name(void *data, struct zwp_tablet_v2 *zwp_tablet_v2,
                             const char *name)
{
	struct tablet_info *tablet_info = data;
	tablet_info->name = xstrdup(name);
}

static void
handle_tablet_v2_tablet_path(void *data, struct zwp_tablet_v2 *zwp_tablet_v2,
                             const char *path)
{
	struct tablet_info *tablet_info = data;
	struct tablet_v2_path *path_elem = xzalloc(sizeof *path_elem);
	path_elem->path = xstrdup(path);

	wl_list_insert(&tablet_info->paths, &path_elem->link);
}

static void
handle_tablet_v2_tablet_id(void *data, struct zwp_tablet_v2 *zwp_tablet_v2,
                           uint32_t vid, uint32_t pid)
{
	struct tablet_info *tablet_info = data;

	tablet_info->vid = vid;
	tablet_info->pid = pid;
}

static void
handle_tablet_v2_tablet_done(void *data, struct zwp_tablet_v2 *zwp_tablet_v2)
{
	/* don't bother waiting for this; there's no good reason a
	 * compositor will wait more than one roundtrip before sending
	 * these initial events. */
}

static void
handle_tablet_v2_tablet_removed(void *data, struct zwp_tablet_v2 *zwp_tablet_v2)
{
	/* don't bother waiting for this; We never make any request that's not
	 * allowed to be issued either way. */
}

static const struct zwp_tablet_v2_listener tablet_listener = {
	.name = handle_tablet_v2_tablet_name,
	.id = handle_tablet_v2_tablet_id,
	.path = handle_tablet_v2_tablet_path,
	.done = handle_tablet_v2_tablet_done,
	.removed = handle_tablet_v2_tablet_removed
};

static void
add_tablet_v2_tablet_info(void *data, struct zwp_tablet_seat_v2 *tablet_seat_v2,
                          struct zwp_tablet_v2 *tablet)
{
	struct tablet_seat_info *tablet_seat = data;
	struct tablet_info *tablet_info = xzalloc(sizeof *tablet_info);

	wl_list_init(&tablet_info->paths);
	tablet_info->tablet = tablet;
	wl_list_insert(&tablet_seat->tablets, &tablet_info->link);

	zwp_tablet_v2_add_listener(tablet, &tablet_listener, tablet_info);
}

static const struct zwp_tablet_seat_v2_listener tablet_seat_listener =  {
	.tablet_added = add_tablet_v2_tablet_info,
	.pad_added = add_tablet_v2_tablet_pad_info,
	.tool_added = add_tablet_v2_tablet_tool_info,
};

static void
add_tablet_seat_info(struct tablet_v2_info *tablet_info, struct seat_info *seat)
{
	struct tablet_seat_info *tablet_seat = xzalloc(sizeof *tablet_seat);

	wl_list_insert(&tablet_info->seats, &tablet_seat->link);
	tablet_seat->seat = zwp_tablet_manager_v2_get_tablet_seat(
		tablet_info->manager, seat->seat);
	zwp_tablet_seat_v2_add_listener(tablet_seat->seat,
		&tablet_seat_listener, tablet_seat);

	wl_list_init(&tablet_seat->pads);
	wl_list_init(&tablet_seat->tablets);
	wl_list_init(&tablet_seat->tools);
	tablet_seat->seat_info = seat;

	tablet_info->info->roundtrip_needed = true;
}

static void
add_tablet_v2_info(struct weston_info *info, uint32_t id, uint32_t version)
{
	struct seat_info *seat;
	struct tablet_v2_info *tablet = xzalloc(sizeof *tablet);

	wl_list_init(&tablet->seats);
	tablet->info = info;

	init_global_info(info, &tablet->global, id,
		zwp_tablet_manager_v2_interface.name, version);
	tablet->global.print = print_tablet_v2_info;
	tablet->global.destroy = destroy_tablet_v2_info;

	tablet->manager = wl_registry_bind(info->registry,
		id, &zwp_tablet_manager_v2_interface, 1);

	wl_list_for_each(seat, &info->seats, global_link) {
		add_tablet_seat_info(tablet, seat);
	}

	info->tablet_info = tablet;
}

static void
add_seat_info(struct weston_info *info, uint32_t id, uint32_t version)
{
	struct seat_info *seat = xzalloc(sizeof *seat);

	/* required to set roundtrip_needed to true in capabilities
	 * handler */
	seat->info = info;

	init_global_info(info, &seat->global, id, "wl_seat", version);
	seat->global.print = print_seat_info;
	seat->global.destroy = destroy_seat_info;

	seat->seat = wl_registry_bind(info->registry,
				      id, &wl_seat_interface, MIN(version, 4));
	wl_seat_add_listener(seat->seat, &seat_listener, seat);

	seat->repeat_rate = seat->repeat_delay = -1;

	info->roundtrip_needed = true;
	wl_list_insert(&info->seats, &seat->global_link);

	if (info->tablet_info) {
		add_tablet_seat_info(info->tablet_info, seat);
	}
}

static void
shm_handle_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
	struct shm_info *shm = data;
	struct shm_format *shm_format = xzalloc(sizeof *shm_format);

	wl_list_insert(&shm->formats, &shm_format->link);
	shm_format->format = format;
}

static const struct wl_shm_listener shm_listener = {
	shm_handle_format,
};

static void
destroy_shm_info(void *data)
{
	struct shm_info *shm = data;
	struct shm_format *format, *tmp;

	wl_list_for_each_safe(format, tmp, &shm->formats, link) {
		wl_list_remove(&format->link);
		free(format);
	}

	wl_shm_destroy(shm->shm);
}

static void
add_shm_info(struct weston_info *info, uint32_t id, uint32_t version)
{
	struct shm_info *shm = xzalloc(sizeof *shm);

	init_global_info(info, &shm->global, id, "wl_shm", version);
	shm->global.print = print_shm_info;
	shm->global.destroy = destroy_shm_info;

	wl_list_init(&shm->formats);

	shm->shm = wl_registry_bind(info->registry,
				    id, &wl_shm_interface, 1);
	wl_shm_add_listener(shm->shm, &shm_listener, shm);

	info->roundtrip_needed = true;
}

static void
linux_dmabuf_handle_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1, uint32_t format)
{
	/* This is a deprecated event, don’t use it. */
}

static void
linux_dmabuf_handle_modifier(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1, uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
	struct linux_dmabuf_info *dmabuf = data;
	struct linux_dmabuf_modifier *linux_dmabuf_modifier = xzalloc(sizeof *linux_dmabuf_modifier);

	wl_list_insert(&dmabuf->modifiers, &linux_dmabuf_modifier->link);
	linux_dmabuf_modifier->format = format;
	linux_dmabuf_modifier->modifier = ((uint64_t)modifier_hi) << 32 | modifier_lo;
}

static const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_listener = {
	linux_dmabuf_handle_format,
	linux_dmabuf_handle_modifier,
};

static void
destroy_linux_dmabuf_info(void *data)
{
	struct linux_dmabuf_info *dmabuf = data;
	struct linux_dmabuf_modifier *modifier, *tmp;

	wl_list_for_each_safe(modifier, tmp, &dmabuf->modifiers, link) {
		wl_list_remove(&modifier->link);
		free(modifier);
	}

	zwp_linux_dmabuf_v1_destroy(dmabuf->dmabuf);
}

static void
add_linux_dmabuf_info(struct weston_info *info, uint32_t id, uint32_t version)
{
	struct linux_dmabuf_info *dmabuf = xzalloc(sizeof *dmabuf);

	init_global_info(info, &dmabuf->global, id, "zwp_linux_dmabuf_v1", version);
	dmabuf->global.print = print_linux_dmabuf_info;
	dmabuf->global.destroy = destroy_linux_dmabuf_info;

	wl_list_init(&dmabuf->modifiers);

	if (version >= 3) {
		dmabuf->dmabuf = wl_registry_bind(info->registry,
		                                  id, &zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(dmabuf->dmabuf, &linux_dmabuf_listener, dmabuf);

		info->roundtrip_needed = true;
	}
}

static void
output_handle_geometry(void *data, struct wl_output *wl_output,
		       int32_t x, int32_t y,
		       int32_t physical_width, int32_t physical_height,
		       int32_t subpixel,
		       const char *make, const char *model,
		       int32_t output_transform)
{
	struct output_info *output = data;

	output->geometry.x = x;
	output->geometry.y = y;
	output->geometry.physical_width = physical_width;
	output->geometry.physical_height = physical_height;
	output->geometry.subpixel = subpixel;
	output->geometry.make = xstrdup(make);
	output->geometry.model = xstrdup(model);
	output->geometry.output_transform = output_transform;
}

static void
output_handle_mode(void *data, struct wl_output *wl_output,
		   uint32_t flags, int32_t width, int32_t height,
		   int32_t refresh)
{
	struct output_info *output = data;
	struct output_mode *mode = xmalloc(sizeof *mode);

	mode->flags = flags;
	mode->width = width;
	mode->height = height;
	mode->refresh = refresh;

	wl_list_insert(output->modes.prev, &mode->link);
}

static void
output_handle_done(void *data, struct wl_output *wl_output)
{
	/* don't bother waiting for this; there's no good reason a
	 * compositor will wait more than one roundtrip before sending
	 * these initial events. */
}

static void
output_handle_scale(void *data, struct wl_output *wl_output,
		    int32_t scale)
{
	struct output_info *output = data;

	output->geometry.scale = scale;
}

static const struct wl_output_listener output_listener = {
	output_handle_geometry,
	output_handle_mode,
	output_handle_done,
	output_handle_scale,
};

static void
destroy_output_info(void *data)
{
	struct output_info *output = data;
	struct output_mode *mode, *tmp;

	wl_output_destroy(output->output);

	if (output->geometry.make != NULL)
		free(output->geometry.make);
	if (output->geometry.model != NULL)
		free(output->geometry.model);

	wl_list_for_each_safe(mode, tmp, &output->modes, link) {
		wl_list_remove(&mode->link);
		free(mode);
	}
}

static void
add_output_info(struct weston_info *info, uint32_t id, uint32_t version)
{
	struct output_info *output = xzalloc(sizeof *output);

	init_global_info(info, &output->global, id, "wl_output", version);
	output->global.print = print_output_info;
	output->global.destroy = destroy_output_info;

	output->version = MIN(version, 2);
	output->geometry.scale = 1;
	wl_list_init(&output->modes);

	output->output = wl_registry_bind(info->registry, id,
					  &wl_output_interface, output->version);
	wl_output_add_listener(output->output, &output_listener,
			       output);

	info->roundtrip_needed = true;
}

static void
destroy_presentation_info(void *info)
{
	struct presentation_info *prinfo = info;

	wp_presentation_destroy(prinfo->presentation);
}

static const char *
clock_name(clockid_t clk_id)
{
	static const char *names[] = {
		[CLOCK_REALTIME] =		"CLOCK_REALTIME",
		[CLOCK_MONOTONIC] =		"CLOCK_MONOTONIC",
		[CLOCK_MONOTONIC_RAW] =		"CLOCK_MONOTONIC_RAW",
		[CLOCK_REALTIME_COARSE] =	"CLOCK_REALTIME_COARSE",
		[CLOCK_MONOTONIC_COARSE] =	"CLOCK_MONOTONIC_COARSE",
#ifdef CLOCK_BOOTTIME
		[CLOCK_BOOTTIME] =		"CLOCK_BOOTTIME",
#endif
	};

	if (clk_id < 0 || (unsigned)clk_id >= ARRAY_LENGTH(names))
		return "unknown";

	return names[clk_id];
}

static void
print_presentation_info(void *info)
{
	struct presentation_info *prinfo = info;

	print_global_info(info);

	printf("\tpresentation clock id: %d (%s)\n",
		prinfo->clk_id, clock_name(prinfo->clk_id));
}

static void
presentation_handle_clock_id(void *data, struct wp_presentation *presentation,
			     uint32_t clk_id)
{
	struct presentation_info *prinfo = data;

	prinfo->clk_id = clk_id;
}

static const struct wp_presentation_listener presentation_listener = {
	presentation_handle_clock_id
};

static void
add_presentation_info(struct weston_info *info, uint32_t id, uint32_t version)
{
	struct presentation_info *prinfo = xzalloc(sizeof *prinfo);

	init_global_info(info, &prinfo->global, id,
			 wp_presentation_interface.name, version);
	prinfo->global.print = print_presentation_info;
	prinfo->global.destroy = destroy_presentation_info;

	prinfo->clk_id = -1;
	prinfo->presentation = wl_registry_bind(info->registry, id,
						&wp_presentation_interface, 1);
	wp_presentation_add_listener(prinfo->presentation,
				     &presentation_listener, prinfo);

	info->roundtrip_needed = true;
}

static void
destroy_global_info(void *data)
{
}

static void
add_global_info(struct weston_info *info, uint32_t id,
		const char *interface, uint32_t version)
{
	struct global_info *global = xzalloc(sizeof *global);

	init_global_info(info, global, id, interface, version);
	global->print = print_global_info;
	global->destroy = destroy_global_info;
}

static void
global_handler(void *data, struct wl_registry *registry, uint32_t id,
	       const char *interface, uint32_t version)
{
	struct weston_info *info = data;

	if (!strcmp(interface, "wl_seat"))
		add_seat_info(info, id, version);
	else if (!strcmp(interface, "wl_shm"))
		add_shm_info(info, id, version);
	else if (!strcmp(interface, "zwp_linux_dmabuf_v1"))
		add_linux_dmabuf_info(info, id, version);
	else if (!strcmp(interface, "wl_output"))
		add_output_info(info, id, version);
	else if (!strcmp(interface, wp_presentation_interface.name))
		add_presentation_info(info, id, version);
	else if (!strcmp(interface, zwp_tablet_manager_v2_interface.name))
		add_tablet_v2_info(info, id, version);
	else
		add_global_info(info, id, interface, version);
}

static void
global_remove_handler(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	global_handler,
	global_remove_handler
};

static void
print_infos(struct wl_list *infos)
{
	struct global_info *info;

	wl_list_for_each(info, infos, link)
		info->print(info);
}

static void
destroy_info(void *data)
{
	struct global_info *global = data;

	global->destroy(data);
	wl_list_remove(&global->link);
	free(global->interface);
	free(data);
}

static void
destroy_infos(struct wl_list *infos)
{
	struct global_info *info, *tmp;
	wl_list_for_each_safe(info, tmp, infos, link)
		destroy_info(info);
}

int
main(int argc, char **argv)
{
	struct weston_info info;

	info.display = wl_display_connect(NULL);
	if (!info.display) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	info.tablet_info = NULL;
	wl_list_init(&info.infos);
	wl_list_init(&info.seats);

	info.registry = wl_display_get_registry(info.display);
	wl_registry_add_listener(info.registry, &registry_listener, &info);

	do {
		info.roundtrip_needed = false;
		wl_display_roundtrip(info.display);
	} while (info.roundtrip_needed);

	print_infos(&info.infos);
	destroy_infos(&info.infos);

	wl_registry_destroy(info.registry);
	wl_display_disconnect(info.display);

	return 0;
}
