/*
 * Copyright © 2008 Kristian Høgsberg
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

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>

#include <GL/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <linux/input.h>
#include <wayland-client.h>

#include "window.h"

struct gears {
	struct window *window;
	struct widget *widget;

	struct display *d;

	EGLDisplay display;
	EGLDisplay config;
	EGLContext context;
	GLfloat angle;

	struct {
		GLfloat rotx;
		GLfloat roty;
	} view;

	int button_down;
	int last_x, last_y;

	GLint gear_list[3];
	int fullscreen;
	int frames;
	uint32_t last_fps;
};

struct gear_template {
	GLfloat material[4];
	GLfloat inner_radius;
	GLfloat outer_radius;
	GLfloat width;
	GLint teeth;
	GLfloat tooth_depth;
};

static const struct gear_template gear_templates[] = {
	{ { 0.8, 0.1, 0.0, 1.0 }, 1.0, 4.0, 1.0, 20, 0.7 },
	{ { 0.0, 0.8, 0.2, 1.0 }, 0.5, 2.0, 2.0, 10, 0.7 },
	{ { 0.2, 0.2, 1.0, 1.0 }, 1.3, 2.0, 0.5, 10, 0.7 },
};

static GLfloat light_pos[4] = {5.0, 5.0, 10.0, 0.0};

static void die(const char *msg)
{
	fprintf(stderr, "%s", msg);
	exit(EXIT_FAILURE);
}

static void
make_gear(const struct gear_template *t)
{
	GLint i;
	GLfloat r0, r1, r2;
	GLfloat angle, da;
	GLfloat u, v, len;

	glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, t->material);

	r0 = t->inner_radius;
	r1 = t->outer_radius - t->tooth_depth / 2.0;
	r2 = t->outer_radius + t->tooth_depth / 2.0;

	da = 2.0 * M_PI / t->teeth / 4.0;

	glShadeModel(GL_FLAT);

	glNormal3f(0.0, 0.0, 1.0);

	/* draw front face */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;
		glVertex3f(r0 * cos(angle), r0 * sin(angle), t->width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), t->width * 0.5);
		if (i < t->teeth) {
			glVertex3f(r0 * cos(angle), r0 * sin(angle), t->width * 0.5);
			glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), t->width * 0.5);
		}
	}
	glEnd();

	/* draw front sides of teeth */
	glBegin(GL_QUADS);
	da = 2.0 * M_PI / t->teeth / 4.0;
	for (i = 0; i < t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;

		glVertex3f(r1 * cos(angle), r1 * sin(angle), t->width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), t->width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), t->width * 0.5);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), t->width * 0.5);
	}
	glEnd();

	glNormal3f(0.0, 0.0, -1.0);

	/* draw back face */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -t->width * 0.5);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), -t->width * 0.5);
		if (i < t->teeth) {
			glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -t->width * 0.5);
			glVertex3f(r0 * cos(angle), r0 * sin(angle), -t->width * 0.5);
		}
	}
	glEnd();

	/* draw back sides of teeth */
	glBegin(GL_QUADS);
	da = 2.0 * M_PI / t->teeth / 4.0;
	for (i = 0; i < t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;

		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -t->width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), -t->width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -t->width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -t->width * 0.5);
	}
	glEnd();

	/* draw outward faces of teeth */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i < t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;

		glVertex3f(r1 * cos(angle), r1 * sin(angle), t->width * 0.5);
		glVertex3f(r1 * cos(angle), r1 * sin(angle), -t->width * 0.5);
		u = r2 * cos(angle + da) - r1 * cos(angle);
		v = r2 * sin(angle + da) - r1 * sin(angle);
		len = sqrt(u * u + v * v);
		u /= len;
		v /= len;
		glNormal3f(v, -u, 0.0);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), t->width * 0.5);
		glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -t->width * 0.5);
		glNormal3f(cos(angle), sin(angle), 0.0);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), t->width * 0.5);
		glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), -t->width * 0.5);
		u = r1 * cos(angle + 3 * da) - r2 * cos(angle + 2 * da);
		v = r1 * sin(angle + 3 * da) - r2 * sin(angle + 2 * da);
		glNormal3f(v, -u, 0.0);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), t->width * 0.5);
		glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -t->width * 0.5);
		glNormal3f(cos(angle), sin(angle), 0.0);
	}

	glVertex3f(r1 * cos(0), r1 * sin(0), t->width * 0.5);
	glVertex3f(r1 * cos(0), r1 * sin(0), -t->width * 0.5);

	glEnd();

	glShadeModel(GL_SMOOTH);

	/* draw inside radius cylinder */
	glBegin(GL_QUAD_STRIP);
	for (i = 0; i <= t->teeth; i++) {
		angle = i * 2.0 * M_PI / t->teeth;
		glNormal3f(-cos(angle), -sin(angle), 0.0);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), -t->width * 0.5);
		glVertex3f(r0 * cos(angle), r0 * sin(angle), t->width * 0.5);
	}
	glEnd();
}

static void
update_fps(struct gears *gears, uint32_t time)
{
	long diff_ms;
	static bool first_call = true;

	if (first_call) {
		gears->last_fps = time;
		first_call = false;
	} else
		gears->frames++;

	diff_ms = time - gears->last_fps;

	if (diff_ms > 5000) {
		float seconds = diff_ms / 1000.0;
		float fps = gears->frames / seconds;

		printf("%d frames in %6.3f seconds = %6.3f FPS\n", gears->frames, seconds, fps);
		fflush(stdout);

		gears->frames = 0;
		gears->last_fps = time;
	}
}

static void
frame_callback(void *data, struct wl_callback *callback, uint32_t time)
{
	struct gears *gears = data;

	update_fps(gears, time);

	gears->angle = (GLfloat) (time % 8192) * 360 / 8192.0;

	window_schedule_redraw(gears->window);

	if (callback)
		wl_callback_destroy(callback);
}

static const struct wl_callback_listener listener = {
	frame_callback
};

static int
motion_handler(struct widget *widget, struct input *input,
		uint32_t time, float x, float y, void *data)
{
	struct gears *gears = data;
	int offset_x, offset_y;
	float step = 0.5;

	if (gears->button_down) {
		offset_x = x - gears->last_x;
		offset_y = y - gears->last_y;
		gears->last_x = x;
		gears->last_y = y;
		gears->view.roty += offset_x * step;
		gears->view.rotx += offset_y * step;
		if (gears->view.roty >= 360)
			gears->view.roty = gears->view.roty - 360;
		if (gears->view.roty <= 0)
			gears->view.roty = gears->view.roty + 360;
		if (gears->view.rotx >= 360)
			gears->view.rotx = gears->view.rotx - 360;
		if (gears->view.rotx <= 0)
			gears->view.rotx = gears->view.rotx + 360;
	}

	return CURSOR_LEFT_PTR;
}

static void
button_handler(struct widget *widget, struct input *input,
		uint32_t time, uint32_t button,
		enum wl_pointer_button_state state, void *data)
{
	struct gears *gears = data;

	if (button == BTN_LEFT) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			gears->button_down = 1;
			input_get_position(input,
					&gears->last_x, &gears->last_y);
		} else {
			gears->button_down = 0;
		}
	}
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct rectangle window_allocation;
	struct rectangle allocation;
	struct wl_callback *callback;
	struct gears *gears = data;

	widget_get_allocation(gears->widget, &allocation);
	window_get_allocation(gears->window, &window_allocation);

	if (display_acquire_window_surface(gears->d,
					    gears->window,
					    gears->context) < 0) {
		die("Unable to acquire window surface, "
		    "compiled without cairo-egl?\n");
	}

	glViewport(allocation.x,
		   window_allocation.height - allocation.height - allocation.y,
		   allocation.width, allocation.height);
	glScissor(allocation.x,
		  window_allocation.height - allocation.height - allocation.y,
		  allocation.width, allocation.height);

	glEnable(GL_SCISSOR_TEST);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glPushMatrix();

	glTranslatef(0.0, 0.0, -50);

	glRotatef(gears->view.rotx, 1.0, 0.0, 0.0);
	glRotatef(gears->view.roty, 0.0, 1.0, 0.0);

	glPushMatrix();
	glTranslatef(-3.0, -2.0, 0.0);
	glRotatef(gears->angle, 0.0, 0.0, 1.0);
	glCallList(gears->gear_list[0]);
	glPopMatrix();

	glPushMatrix();
	glTranslatef(3.1, -2.0, 0.0);
	glRotatef(-2.0 * gears->angle - 9.0, 0.0, 0.0, 1.0);
	glCallList(gears->gear_list[1]);
	glPopMatrix();

	glPushMatrix();
	glTranslatef(-3.1, 4.2, 0.0);
	glRotatef(-2.0 * gears->angle - 25.0, 0.0, 0.0, 1.0);
	glCallList(gears->gear_list[2]);
	glPopMatrix();

	glPopMatrix();

	glFlush();

	display_release_window_surface(gears->d, gears->window);

	callback = wl_surface_frame(window_get_wl_surface(gears->window));
	wl_callback_add_listener(callback, &listener, gears);
}

static void
resize_handler(struct widget *widget,
	       int32_t width, int32_t height, void *data)
{
	struct gears *gears = data;
	int32_t size, big, small;

	/* Constrain child size to be square and at least 300x300 */
	if (width < height) {
		small = width;
		big = height;
	} else {
		small = height;
		big = width;
	}

	if (gears->fullscreen)
		size = small;
	else
		size = big;

	widget_set_size(gears->widget, size, size);
}

static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data)
{
	window_schedule_redraw(window);
}

static void
fullscreen_handler(struct window *window, void *data)
{
	struct gears *gears = data;

	gears->fullscreen ^= 1;
	window_set_fullscreen(window, gears->fullscreen);
}

static struct gears *
gears_create(struct display *display)
{
	const int width = 450, height = 500;
	struct gears *gears;
	int i;

	gears = zalloc(sizeof *gears);
	if (!gears)
		return NULL;

	gears->d = display;
	gears->window = window_create(display);
	gears->widget = window_frame_create(gears->window, gears);
	window_set_title(gears->window, "Wayland Gears");

	gears->display = display_get_egl_display(gears->d);
	if (gears->display == NULL)
		die("failed to create egl display\n");

	eglBindAPI(EGL_OPENGL_API);

	gears->config = display_get_argb_egl_config(gears->d);

	gears->context = eglCreateContext(gears->display, gears->config,
					  EGL_NO_CONTEXT, NULL);
	if (gears->context == NULL)
		die("failed to create context\n");

	if (!eglMakeCurrent(gears->display, NULL, NULL, gears->context))
		die("failed to make context current\n");

	for (i = 0; i < 3; i++) {
		gears->gear_list[i] = glGenLists(1);
		glNewList(gears->gear_list[i], GL_COMPILE);
		make_gear(&gear_templates[i]);
		glEndList();
	}

	gears->button_down = 0;
	gears->last_x = 0;
	gears->last_y = 0;

	gears->view.rotx = 20.0;
	gears->view.roty = 30.0;

	printf("Warning: FPS count is limited by the wayland compositor or monitor refresh rate\n");

	glEnable(GL_NORMALIZE);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1.0, 1.0, -1.0, 1.0, 5.0, 200.0);
	glMatrixMode(GL_MODELVIEW);

	glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
	glEnable(GL_CULL_FACE);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 0.92);

	window_set_user_data(gears->window, gears);
	widget_set_resize_handler(gears->widget, resize_handler);
	widget_set_redraw_handler(gears->widget, redraw_handler);
	widget_set_button_handler(gears->widget, button_handler);
	widget_set_motion_handler(gears->widget, motion_handler);
	window_set_keyboard_focus_handler(gears->window,
					  keyboard_focus_handler);
	window_set_fullscreen_handler(gears->window, fullscreen_handler);

	window_schedule_resize(gears->window, width, height);

	return gears;
}

static void
gears_destroy(struct gears *gears)
{
	widget_destroy(gears->widget);
	window_destroy(gears->window);
	free(gears);
}

int main(int argc, char *argv[])
{
	struct display *d;
	struct gears *gears;

	d = display_create(&argc, argv);
	if (d == NULL) {
		fprintf(stderr, "failed to create display: %s\n",
			strerror(errno));
		return -1;
	}
	gears = gears_create(d);
	display_run(d);

	gears_destroy(gears);
	display_destroy(d);

	return 0;
}
