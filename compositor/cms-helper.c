/*
 * Copyright © 2013 Richard Hughes
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

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#ifdef HAVE_LCMS
#include <lcms2.h>
#endif

#include <libweston/libweston.h>
#include "cms-helper.h"

#ifdef HAVE_LCMS
static void
weston_cms_gamma_clear(struct weston_output *o)
{
	int i;
	uint16_t *red;

	if (!o->set_gamma)
		return;

	red = calloc(o->gamma_size, sizeof(uint16_t));
	for (i = 0; i < o->gamma_size; i++)
		red[i] = (uint32_t) 0xffff * (uint32_t) i / (uint32_t) (o->gamma_size - 1);
	o->set_gamma(o, o->gamma_size, red, red, red);
	free(red);
}
#endif

void
weston_cms_set_color_profile(struct weston_output *o,
			     struct weston_color_profile *p)
{
#ifdef HAVE_LCMS
	cmsFloat32Number in;
	const cmsToneCurve **vcgt;
	int i;
	int size;
	uint16_t *red = NULL;
	uint16_t *green = NULL;
	uint16_t *blue = NULL;

	if (!o->set_gamma)
		return;
	if (!p) {
		weston_cms_gamma_clear(o);
		return;
	}

	weston_log("Using ICC profile %s\n", p->filename);
	vcgt = cmsReadTag (p->lcms_handle, cmsSigVcgtTag);
	if (vcgt == NULL || vcgt[0] == NULL) {
		weston_cms_gamma_clear(o);
		return;
	}

	size = o->gamma_size;
	red = calloc(size, sizeof(uint16_t));
	green = calloc(size, sizeof(uint16_t));
	blue = calloc(size, sizeof(uint16_t));
	for (i = 0; i < size; i++) {
		in = (cmsFloat32Number) i / (cmsFloat32Number) (size - 1);
		red[i] = cmsEvalToneCurveFloat(vcgt[0], in) * (double) 0xffff;
		green[i] = cmsEvalToneCurveFloat(vcgt[1], in) * (double) 0xffff;
		blue[i] = cmsEvalToneCurveFloat(vcgt[2], in) * (double) 0xffff;
	}
	o->set_gamma(o, size, red, green, blue);
	free(red);
	free(green);
	free(blue);
#endif
}

void
weston_cms_destroy_profile(struct weston_color_profile *p)
{
	if (!p)
		return;
#ifdef HAVE_LCMS
	cmsCloseProfile(p->lcms_handle);
#endif
	free(p->filename);
	free(p);
}

struct weston_color_profile *
weston_cms_create_profile(const char *filename,
			  void *lcms_profile)
{
	struct weston_color_profile *p;
	p = zalloc(sizeof(struct weston_color_profile));
	if (!p)
		return NULL;

	p->filename = strdup(filename);
	p->lcms_handle = lcms_profile;
	return p;
}

struct weston_color_profile *
weston_cms_load_profile(const char *filename)
{
	struct weston_color_profile *p = NULL;
#ifdef HAVE_LCMS
	cmsHPROFILE lcms_profile;
	lcms_profile = cmsOpenProfileFromFile(filename, "r");
	if (lcms_profile)
		p = weston_cms_create_profile(filename, lcms_profile);
#endif
	return p;
}
