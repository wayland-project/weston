m4_define([weston_major_version], [4])
m4_define([weston_minor_version], [0])
m4_define([weston_micro_version], [90])
m4_define([weston_version],
          [weston_major_version.weston_minor_version.weston_micro_version])
m4_define([libweston_major_version], [5])
m4_define([libweston_minor_version], [0])
m4_define([libweston_patch_version], [0])

AC_PREREQ([2.64])
AC_INIT([weston],
        [weston_version],
        [https://bugs.freedesktop.org/enter_bug.cgi?product=Wayland&component=weston&version=weston_version],
        [weston],
        [http://wayland.freedesktop.org])

WAYLAND_PREREQ_VERSION="1.12.0"

AC_SUBST([WESTON_VERSION_MAJOR], [weston_major_version])
AC_SUBST([WESTON_VERSION_MINOR], [weston_minor_version])
AC_SUBST([WESTON_VERSION_MICRO], [weston_micro_version])
AC_SUBST([WESTON_VERSION], [weston_version])
AC_SUBST([LIBWESTON_MAJOR], [libweston_major_version])
# We use minor as current and age since on ABI/API break/removal we bump major
# so minor will be reset to 0.
m4_define([lt_current], [libweston_minor_version])
m4_define([lt_revision], [libweston_patch_version])
m4_define([lt_age], [libweston_minor_version])
AC_SUBST([LT_VERSION_INFO], [lt_current:lt_revision:lt_age])

AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

save_CFLAGS="$CFLAGS"
export CFLAGS="$CFLAGS -Werror"
AC_HEADER_MAJOR
CFLAGS="$save_CFLAGS"

AM_INIT_AUTOMAKE([1.11 parallel-tests foreign no-dist-gzip dist-xz color-tests subdir-objects])

AM_SILENT_RULES([yes])

# Check Weston and libweston version consistency
m4_if(m4_cmp(weston_micro_version, [90]), [-1],
      [
	dnl micro < 90
	dnl A final or stable release, not a pre-release:
	dnl Weston and libweston versions must match.
	m4_if(weston_version, libweston_major_version[.]libweston_minor_version[.]libweston_patch_version,
	      [],
	      [AC_MSG_ERROR([Weston and libweston version mismatch for a final release])])
      ],
      [
	dnl A pre-release:
	dnl libweston must be equal or greater than weston.
	m4_case(m4_list_cmp([weston_major_version, weston_minor_version, weston_micro_version],
			     [libweston_major_version, libweston_minor_version, libweston_patch_version]),
		 [-1], [
			dnl weston < libweston
			dnl libweston must be weston_major+1.0.0
			m4_if(m4_eval(weston_major_version+1)[.0.0],
			      libweston_major_version[.]libweston_minor_version[.]libweston_patch_version,
			      [],
			      [AC_MSG_ERROR([libweston version is greater but not (weston_major+1).0.0])])
		 ],
		 [0], [
			dnl weston == libweston, all ok
		 ],
		 [1], [
			dnl weston > libweston, wrong
			AC_MSG_ERROR([Weston version is greater than libweston.])
		 ])
       ])

# Check for programs
AC_PROG_CC
AC_PROG_SED

# Initialize libtool
LT_PREREQ([2.2])
LT_INIT([disable-static])

AC_ARG_VAR([WESTON_NATIVE_BACKEND],
           [Set the native backend to use, if Weston is not running under Wayland nor X11. @<:@default=drm-backend.so@:>@])
AC_ARG_VAR([WESTON_SHELL_CLIENT],
           [Set the default desktop shell client to load if none is specified in weston.ini. @<:@default=weston-desktop-shell@:>@])

PKG_PROG_PKG_CONFIG()

# Check for dlsym instead of dlopen because ASAN hijacks the latter
WESTON_SEARCH_LIBS([DL], [dl], [dlsym])

# In old glibc versions (< 2.17) clock_gettime() and clock_getres() are in librt
WESTON_SEARCH_LIBS([CLOCK_GETTIME], [rt], [clock_gettime])
WESTON_SEARCH_LIBS([CLOCK_GETRES], [rt], [clock_getres])

AC_CHECK_DECL(SFD_CLOEXEC,[],
	      [AC_MSG_ERROR("SFD_CLOEXEC is needed to compile weston")],
	      [[#include <sys/signalfd.h>]])
AC_CHECK_DECL(TFD_CLOEXEC,[],
	      [AC_MSG_ERROR("TFD_CLOEXEC is needed to compile weston")],
	      [[#include <sys/timerfd.h>]])
AC_CHECK_DECL(CLOCK_MONOTONIC,[],
	      [AC_MSG_ERROR("CLOCK_MONOTONIC is needed to compile weston")],
	      [[#include <time.h>]])
AC_CHECK_HEADERS([execinfo.h])

AC_CHECK_FUNCS([mkostemp strchrnul initgroups posix_fallocate])

# check for libdrm as a build-time dependency only
# libdrm 2.4.30 introduced drm_fourcc.h.
PKG_CHECK_MODULES(LIBDRM, [libdrm >= 2.4.30], [], [AC_MSG_ERROR([
     libdrm is a hard build-time dependency for libweston core,
     but a sufficient version was not found. However, libdrm
     is not a runtime dependency unless you have features
     enabled that require it.])])

COMPOSITOR_MODULES="wayland-server >= $WAYLAND_PREREQ_VERSION pixman-1 >= 0.25.2"

AC_CONFIG_FILES([doc/doxygen/tools.doxygen doc/doxygen/tooldev.doxygen])

AC_ARG_ENABLE(devdocs,
	      AS_HELP_STRING([--disable-devdocs],
			     [do not enable building of developer documentation]),,
	      enable_devdocs=auto)
if test "x$enable_devdocs" != "xno"; then
	AC_CHECK_PROGS([DOXYGEN], [doxygen])
	if test "x$DOXYGEN" = "x" -a "x$enable_devdocs" = "xyes"; then
		AC_MSG_ERROR([Developer documentation explicitly requested, but Doxygen couldn't be found])
	fi
	if test "x$DOXYGEN" != "x"; then
		enable_devdocs=yes
	else
		enable_devdocs=no
	fi
fi
AM_CONDITIONAL(ENABLE_DEVDOCS, test "x$enable_devdocs" = "xyes")

AC_ARG_ENABLE(egl, [  --disable-egl],,
              enable_egl=yes)
AM_CONDITIONAL(ENABLE_EGL, test x$enable_egl = xyes)
if test x$enable_egl = xyes; then
	AC_DEFINE([ENABLE_EGL], [1], [Build Weston with EGL support])
	PKG_CHECK_MODULES(EGL, [egl glesv2])
        PKG_CHECK_MODULES([EGL_TESTS], [egl glesv2 wayland-client wayland-egl])
	AC_CHECK_HEADERS([linux/sync_file.h])
fi

COMPOSITOR_MODULES="$COMPOSITOR_MODULES xkbcommon >= 0.3.0"

PKG_CHECK_MODULES(XKBCOMMON_COMPOSE, [xkbcommon >= 0.5.0],
                  [AC_DEFINE(HAVE_XKBCOMMON_COMPOSE, 1,
	             [Define if xkbcommon is 0.5.0 or newer])],true)

AC_ARG_ENABLE(setuid-install, [  --enable-setuid-install],,
	      enable_setuid_install=yes)
AM_CONDITIONAL(ENABLE_SETUID_INSTALL, test x$enable_setuid_install = xyes)


AC_ARG_ENABLE(xwayland, [  --enable-xwayland],,
	      enable_xwayland=yes)
AC_ARG_ENABLE(xwayland-test, [  --enable-xwayland-test],,
              enable_xwayland_test=yes)
AM_CONDITIONAL(ENABLE_XWAYLAND, test x$enable_xwayland = xyes)
AM_CONDITIONAL(ENABLE_XWAYLAND_TEST, test x$enable_xwayland = xyes -a x$enable_xwayland_test = xyes)
if test x$enable_xwayland = xyes; then
  PKG_CHECK_MODULES([XWAYLAND], xcb xcb-xfixes xcb-composite xcb-shape xcursor cairo-xcb)
  AC_DEFINE([BUILD_XWAYLAND], [1], [Build the X server launcher])

  AC_ARG_WITH(xserver-path, AS_HELP_STRING([--with-xserver-path=PATH],
              [Path to X server]), [XSERVER_PATH="$withval"],
              [XSERVER_PATH="/usr/bin/Xwayland"])
  AC_SUBST([XSERVER_PATH])
  if test x$enable_xwayland_test = xyes; then
    PKG_CHECK_MODULES([XWAYLAND_TEST], x11)
  fi
fi

AC_ARG_ENABLE(x11-compositor, [  --enable-x11-compositor],,
	      enable_x11_compositor=yes)
AM_CONDITIONAL(ENABLE_X11_COMPOSITOR, test x$enable_x11_compositor = xyes)
have_xcb_xkb=no
if test x$enable_x11_compositor = xyes; then
  PKG_CHECK_MODULES([XCB], xcb >= 1.8)
  X11_COMPOSITOR_MODULES="x11 x11-xcb xcb-shm"

  PKG_CHECK_MODULES(X11_COMPOSITOR_XKB, [xcb-xkb >= 1.9],
		    [have_xcb_xkb="yes"], [have_xcb_xkb="no"])
  if test "x$have_xcb_xkb" = xyes; then
	X11_COMPOSITOR_MODULES="$X11_COMPOSITOR_MODULES xcb-xkb"
	AC_DEFINE([HAVE_XCB_XKB], [1], [libxcb supports XKB protocol])
  fi

  PKG_CHECK_MODULES(X11_COMPOSITOR, [$X11_COMPOSITOR_MODULES])
  AC_DEFINE([BUILD_X11_COMPOSITOR], [1], [Build the X11 compositor])
fi


AC_ARG_ENABLE(drm-compositor, [  --enable-drm-compositor],,
	      enable_drm_compositor=yes)
AM_CONDITIONAL(ENABLE_DRM_COMPOSITOR, test x$enable_drm_compositor = xyes)
if test x$enable_drm_compositor = xyes; then
  AC_DEFINE([BUILD_DRM_COMPOSITOR], [1], [Build the DRM compositor])
  PKG_CHECK_MODULES(DRM_COMPOSITOR, [libudev >= 136 libdrm >= 2.4.30 gbm])
  PKG_CHECK_MODULES(DRM_COMPOSITOR_ATOMIC, [libdrm >= 2.4.78],
		    [AC_DEFINE([HAVE_DRM_ATOMIC], 1, [libdrm supports atomic API])],
		    [AC_MSG_WARN([libdrm does not support atomic modesetting, will omit that capability])])
  PKG_CHECK_MODULES(DRM_COMPOSITOR_GBM, [gbm >= 10.2],
		    [AC_DEFINE([HAVE_GBM_FD_IMPORT], 1, [gbm supports dmabuf import])],
		    [AC_MSG_WARN([gbm does not support dmabuf import, will omit that capability])])
fi


PKG_CHECK_MODULES(LIBINPUT_BACKEND, [libinput >= 0.8.0])
PKG_CHECK_MODULES(COMPOSITOR, [$COMPOSITOR_MODULES])

PKG_CHECK_MODULES(WAYLAND_PROTOCOLS, [wayland-protocols >= 1.13],
		  [ac_wayland_protocols_pkgdatadir=`$PKG_CONFIG --variable=pkgdatadir wayland-protocols`])
AC_SUBST(WAYLAND_PROTOCOLS_DATADIR, $ac_wayland_protocols_pkgdatadir)

AC_ARG_ENABLE(wayland-compositor, [  --enable-wayland-compositor],,
	      enable_wayland_compositor=yes)
AM_CONDITIONAL(ENABLE_WAYLAND_COMPOSITOR,
	       test x$enable_wayland_compositor = xyes)
if test x$enable_wayland_compositor = xyes; then
  AC_DEFINE([BUILD_WAYLAND_COMPOSITOR], [1],
	    [Build the Wayland (nested) compositor])
  PKG_CHECK_MODULES(WAYLAND_COMPOSITOR, [wayland-client >= $WAYLAND_PREREQ_VERSION wayland-cursor])
  if test x$enable_egl = xyes; then
    PKG_CHECK_MODULES(WAYLAND_COMPOSITOR_EGL, [wayland-egl])
  fi
fi


AC_ARG_ENABLE(headless-compositor, [  --enable-headless-compositor],,
	      enable_headless_compositor=yes)
AM_CONDITIONAL(ENABLE_HEADLESS_COMPOSITOR,
	       test x$enable_headless_compositor = xyes)
if test x$enable_headless_compositor = xyes; then
  AC_DEFINE([BUILD_HEADLESS_COMPOSITOR], [1], [Build the headless compositor])
fi


AC_ARG_ENABLE([fbdev-compositor], [  --enable-fbdev-compositor],,
              enable_fbdev_compositor=yes)
AM_CONDITIONAL([ENABLE_FBDEV_COMPOSITOR],
               [test x$enable_fbdev_compositor = xyes])
AS_IF([test x$enable_fbdev_compositor = xyes], [
  AC_DEFINE([BUILD_FBDEV_COMPOSITOR], [1], [Build the fbdev compositor])
  PKG_CHECK_MODULES([FBDEV_COMPOSITOR], [libudev >= 136])
])

AC_ARG_ENABLE([rdp-compositor], [  --enable-rdp-compositor],,
              enable_rdp_compositor=no)
AM_CONDITIONAL([ENABLE_RDP_COMPOSITOR],
               [test x$enable_rdp_compositor = xyes])
if test x$enable_rdp_compositor = xyes; then
  AC_DEFINE([BUILD_RDP_COMPOSITOR], [1], [Build the RDP compositor])
  PKG_CHECK_MODULES(RDP_COMPOSITOR, [freerdp2 >= 2.0.0],
    [],
    [PKG_CHECK_MODULES(RDP_COMPOSITOR, [freerdp >= 1.1.0],[])]
  )
  SAVED_CPPFLAGS="$CPPFLAGS"
  CPPFLAGS="$CPPFLAGS $RDP_COMPOSITOR_CFLAGS"

  AC_CHECK_HEADERS([freerdp/version.h])
  AC_CHECK_MEMBER([SURFACE_BITS_COMMAND.bmp],
	[AC_DEFINE([HAVE_SURFACE_BITS_BMP], [1], [SURFACE_BITS_CMD has bmp field])],
	[],
	[[#include <freerdp/update.h>]]
  )


  CPPFLAGS="$SAVED_CPPFLAGS"
fi

AC_ARG_ENABLE([screen-sharing], [  --enable-screen-sharing],,
              enable_screen_sharing=no)
AM_CONDITIONAL([ENABLE_SCREEN_SHARING],
               [test x$enable_screen_sharing = xyes])
if test x$enable_screen_sharing = xyes; then
  PKG_CHECK_MODULES(SCREEN_SHARE, [wayland-client])

  if test x$enable_rdp_compositor != xyes; then
    AC_MSG_WARN([The screen-share.so module requires the RDP backend.])
  fi
fi

AC_ARG_WITH(cairo,
	    AS_HELP_STRING([--with-cairo=@<:@image|gl|glesv2@:>@]
			   [Which Cairo renderer to use for the clients]),
			   [],[with_cairo="image"])

if test "x$with_cairo" = "ximage"; then
	cairo_modules="cairo"
else
if test "x$with_cairo" = "xgl"; then
	cairo_modules="cairo-gl"
	AC_MSG_WARN([The --with-cairo=gl option can cause increased resource usage and potential instability, and thus is not recommended.  It is needed only for a few special demo programs.])
else
if test "x$with_cairo" = "xglesv2"; then
	cairo_modules="cairo-glesv2"
	AC_MSG_WARN([The --with-cairo=gles2 option can cause increased resource usage and potential instability, and thus is not recommended.  It is needed only for a few special demo programs.])
else
	AC_ERROR([Unknown cairo renderer requested])
fi
fi
fi

# Included for legacy compat
AC_ARG_WITH(cairo-glesv2,
            AS_HELP_STRING([--with-cairo-glesv2],
                           [Use GLESv2 cairo]))
if test "x$with_cairo_glesv2" = "xyes"; then
  cairo_modules="cairo-glesv2"
  with_cairo="glesv2"
fi

if test "x$cairo_modules" = "xcairo-glesv2"; then
AC_DEFINE([USE_CAIRO_GLESV2], [1], [Use the GLESv2 GL cairo backend])
fi

PKG_CHECK_MODULES(PIXMAN, [pixman-1])
PKG_CHECK_MODULES(PNG, [libpng])

AC_ARG_WITH([jpeg],
            AS_HELP_STRING([--without-jpeg],
                           [Use jpeglib for JPEG decoding support [default=auto]]))
AS_IF([test "x$with_jpeg" != "xno"],
      [WESTON_SEARCH_LIBS([JPEG], [jpeg], [jpeg_CreateDecompress], [have_jpeglib=yes], [have_jpeglib=no])],
      [have_jpeglib=no])
AS_IF([test "x$have_jpeglib" = "xyes"],
      [AC_DEFINE([HAVE_JPEG], [1], [Have jpeglib])],
      [AS_IF([test "x$with_jpeg" = "xyes"],
             [AC_MSG_ERROR([JPEG support explicitly requested, but jpeglib couldn't be found])])])

AC_ARG_WITH([webp],
            AS_HELP_STRING([--without-webp],
                           [Use libwebp for WebP decoding support [default=auto]]))
AS_IF([test "x$with_webp" != "xno"],
      [PKG_CHECK_MODULES(WEBP, [libwebp], [have_webp=yes], [have_webp=no])],
      [have_webp=no])
AS_IF([test "x$have_webp" = "xyes"],
      [AC_DEFINE([HAVE_WEBP], [1], [Have webp])],
      [AS_IF([test "x$with_webp" = "xyes"],
             [AC_MSG_ERROR([WebP support explicitly requested, but libwebp couldn't be found])])])

AC_ARG_ENABLE(vaapi-recorder, [  --enable-vaapi-recorder],,
	      enable_vaapi_recorder=auto)
have_libva=no
if test x$enable_vaapi_recorder != xno; then
  PKG_CHECK_MODULES(LIBVA, [libva >= 0.34.0 libva-drm >= 0.34.0],
                    [have_libva=yes], [have_libva=no])
  if test "x$have_libva" = "xno" -a "x$enable_vaapi_recorder" = "xyes"; then
    AC_MSG_ERROR([vaapi-recorder explicitly enabled, but libva couldn't be found])
  fi
  AS_IF([test "x$have_libva" = "xyes"],
        [AC_DEFINE([BUILD_VAAPI_RECORDER], [1], [Build the vaapi recorder])])
fi
AM_CONDITIONAL(ENABLE_VAAPI_RECORDER, test "x$have_libva" = xyes)

PKG_CHECK_MODULES(CAIRO, [cairo])

PKG_CHECK_MODULES(TEST_CLIENT, [wayland-client >= $WAYLAND_PREREQ_VERSION pixman-1])

AC_ARG_ENABLE(simple-clients,
              AS_HELP_STRING([--disable-simple-clients],
                             [do not build the simple wl_shm clients]),,
              enable_simple_clients=yes)
AM_CONDITIONAL(BUILD_SIMPLE_CLIENTS, test "x$enable_simple_clients" = "xyes")
if test x$enable_simple_clients = xyes; then
  PKG_CHECK_MODULES(SIMPLE_CLIENT, [wayland-client])
fi

AC_ARG_ENABLE(simple-egl-clients,
              AS_HELP_STRING([--disable-simple-egl-clients],
                             [do not build the simple EGL clients]),,
              enable_simple_egl_clients="$enable_egl")
AM_CONDITIONAL(BUILD_SIMPLE_EGL_CLIENTS, test "x$enable_simple_egl_clients" = "xyes")
if test x$enable_simple_egl_clients = xyes; then
  PKG_CHECK_MODULES(SIMPLE_EGL_CLIENT,
                    [egl glesv2 wayland-client wayland-egl wayland-cursor])
fi

AC_ARG_ENABLE(simple-dmabuf-drm-client,
              AS_HELP_STRING([--disable-simple-dmabuf-drm-client],
                             [do not build the simple dmabuf drm client]),,
              enable_simple_dmabuf_drm_client="auto")
if ! test "x$enable_simple_dmabuf_drm_client" = "xno"; then
  PKG_CHECK_MODULES(SIMPLE_DMABUF_DRM_CLIENT, [wayland-client libdrm egl], [have_simple_dmabuf_libs=yes],
		    [have_simple_dmabuf_libs=no])

  PKG_CHECK_MODULES(LIBDRM_PLATFORM_FREEDRENO, [libdrm_freedreno],
      AC_DEFINE([HAVE_LIBDRM_FREEDRENO], [1], [Build freedreno dmabuf client]) have_simple_dmabuf_drm_client=yes,
      [true])
  PKG_CHECK_MODULES(LIBDRM_PLATFORM_INTEL, [libdrm_intel],
      AC_DEFINE([HAVE_LIBDRM_INTEL], [1], [Build intel dmabuf client]) have_simple_dmabuf_drm_client=yes,
      [true])
  PKG_CHECK_MODULES(LIBDRM_PLATFORM_ETNAVIV, [libdrm_etnaviv],
      AC_DEFINE([HAVE_LIBDRM_ETNAVIV], [1], [Build etnaviv dmabuf client]) have_simple_dmabuf_drm_client=yes,
      [true])

  if test "x$have_simple_dmabuf_drm_client" != "xyes" -o \
	  "x$have_simple_dmabuf_libs" = "xno" && \
     test "x$enable_simple_dmabuf_drm_client" = "xyes"; then
    AC_MSG_ERROR([DRM dmabuf client explicitly enabled, but none of libdrm_{intel,freedreno,etnaviv} found])
  fi

  if test "x$have_simple_dmabuf_drm_client" = "xyes" -a "x$have_simple_dmabuf_libs" = "xyes"; then
    enable_simple_dmabuf_drm_client="yes"
  fi
fi
AM_CONDITIONAL(BUILD_SIMPLE_DMABUF_DRM_CLIENT, test "x$enable_simple_dmabuf_drm_client" = "xyes")

AC_ARG_ENABLE(simple-dmabuf-v4l-client,
              AS_HELP_STRING([--disable-simple-dmabuf-v4l-client],
                             [do not build the simple dmabuf v4l client]),,
              enable_simple_dmabuf_v4l_client="auto")
if ! test "x$enable_simple_dmabuf_v4l_client" = "xno"; then
  PKG_CHECK_MODULES(SIMPLE_DMABUF_V4L_CLIENT, [wayland-client libdrm],
		    have_simple_dmabuf_v4l_client=yes, have_simple_dmabuf_v4l_client=no)
  if test "x$have_simple_dmabuf_v4l_client" = "xno" -a "x$enable_simple_dmabuf_v4l_client" = "xyes"; then
    AC_MSG_ERROR([V4L dmabuf client explicitly enabled, but libdrm couldn't be found])
  fi
  enable_simple_dmabuf_v4l_client="$have_simple_dmabuf_v4l_client"
fi
AM_CONDITIONAL(BUILD_SIMPLE_DMABUF_V4L_CLIENT, test "x$enable_simple_dmabuf_v4l_client" = "xyes")

AC_ARG_ENABLE(clients, [  --enable-clients],, enable_clients=yes)
AM_CONDITIONAL(BUILD_CLIENTS, test x$enable_clients = xyes)
have_cairo_egl=no
if test x$enable_clients = xyes; then
  AC_DEFINE([BUILD_CLIENTS], [1], [Build the Wayland clients])

  PKG_CHECK_MODULES(CLIENT, [wayland-client >= $WAYLAND_PREREQ_VERSION cairo >= 1.10.0 xkbcommon wayland-cursor])
  PKG_CHECK_MODULES(SERVER, [wayland-server])
  PKG_CHECK_MODULES(WESTON_INFO, [wayland-client >= $WAYLAND_PREREQ_VERSION])

  # Only check for cairo-egl if a GL or GLES renderer requested
  AS_IF([test "x$cairo_modules" = "xcairo-gl" -o "x$cairo_modules" = "xcairo-glesv2"], [
    PKG_CHECK_MODULES(CAIRO_EGL, [wayland-egl egl cairo-egl >= 1.11.3 $cairo_modules],
                      [have_cairo_egl=yes], [have_cairo_egl=no])
    AS_IF([test "x$have_cairo_egl" = "xyes"],
	  [AC_DEFINE([HAVE_CAIRO_EGL], [1], [Have cairo-egl])],
	  [AC_ERROR([cairo-egl not used because $CAIRO_EGL_PKG_ERRORS])])],
  [have_cairo_egl=no])

  PKG_CHECK_MODULES(PANGO, [pangocairo pango glib-2.0 >= 2.36], [have_pango=yes], [have_pango=no])
fi

AC_ARG_ENABLE(resize-optimization,
              AS_HELP_STRING([--disable-resize-optimization],
                             [disable resize optimization allocating a big buffer in toytoolkit]),,
              enable_resize_optimization=yes)
AS_IF([test "x$enable_resize_optimization" = "xyes"],
      [AC_DEFINE([USE_RESIZE_POOL], [1], [Use resize memory pool as a performance optimization])])

AC_ARG_ENABLE(weston-launch, [  --enable-weston-launch],, enable_weston_launch=yes)
AM_CONDITIONAL(BUILD_WESTON_LAUNCH, test x$enable_weston_launch = xyes)
if test x$enable_weston_launch = xyes; then
  WESTON_SEARCH_LIBS([PAM], [pam], [pam_open_session], [have_pam=yes], [have_pam=no])
  if test x$have_pam = xno; then
    AC_ERROR([weston-launch requires pam])
  fi
fi

AM_CONDITIONAL(HAVE_PANGO, test "x$have_pango" = "xyes")
if test "x$have_pango" = "xyes"; then
  AC_DEFINE([HAVE_PANGO], [1], [Have pango])
fi

AM_CONDITIONAL(HAVE_CAIRO_GLESV2,
	       [test "x$have_cairo_egl" = "xyes" -a "x$cairo_modules" = "xcairo-glesv2" -a "x$enable_egl" = "xyes"])

AM_CONDITIONAL(BUILD_FULL_GL_CLIENTS,
	       test x$cairo_modules = "xcairo-gl" -a "x$have_cairo_egl" = "xyes" -a "x$enable_egl" = "xyes")

AM_CONDITIONAL(BUILD_SUBSURFACES_CLIENT,
	       [test '(' "x$have_cairo_egl" != "xyes" -o "x$cairo_modules" = "xcairo-glesv2" ')' -a "x$enable_simple_egl_clients" = "xyes"])

AM_CONDITIONAL(ENABLE_DESKTOP_SHELL, true)

AC_ARG_ENABLE(fullscreen-shell,
              AS_HELP_STRING([--disable-fullscreen-shell],
                             [do not build fullscreen-shell server plugin]),,
	      enable_fullscreen_shell=yes)
AM_CONDITIONAL(ENABLE_FULLSCREEN_SHELL,
	       test "x$enable_fullscreen_shell" = "xyes")

# CMS modules
AC_ARG_ENABLE(colord,
              AS_HELP_STRING([--disable-colord],
                             [do not build colord CMS support]),,
	      enable_colord=auto)
have_colord=no
if test "x$enable_colord" != "xno"; then
	PKG_CHECK_MODULES(COLORD,
			  colord >= 0.1.27,
			  have_colord=yes,
			  have_colord=no)
	if test "x$have_colord" = "xno" -a "x$enable_colord" = "xyes"; then
	  AC_MSG_ERROR([colord support explicitly requested, but colord couldn't be found])
	fi
	if test "x$have_colord" = "xyes"; then
	     enable_colord=yes
	fi
fi
AM_CONDITIONAL(ENABLE_COLORD, test "x$enable_colord" = "xyes")

# dbus support
AC_ARG_ENABLE(dbus,
              AS_HELP_STRING([--disable-dbus],
                             [do not build with dbus support]),,
              enable_dbus=auto)
have_dbus=no
if test "x$enable_dbus" != "xno"; then
        PKG_CHECK_MODULES(DBUS,
                          dbus-1 >= 1.6,
                          have_dbus=yes,
                          have_dbus=no)
        if test "x$have_dbus" = "xno" -a "x$enable_dbus" = "xyes"; then
                AC_MSG_ERROR([dbus support explicitly requested, but libdbus couldn't be found])
        fi
        if test "x$have_dbus" = "xyes"; then
                enable_dbus=yes
                AC_DEFINE([HAVE_DBUS], [1], [Build with dbus support])
        else
                enable_dbus=no
        fi
fi
AM_CONDITIONAL(ENABLE_DBUS, test "x$enable_dbus" = "xyes")

# systemd-login support
AC_ARG_ENABLE(systemd-login,
              AS_HELP_STRING([--enable-systemd-login],
                             [Enable logind support]),,
              enable_systemd_login=auto)
if test x$enable_systemd_login != xno -a x$have_dbus != xno; then
  PKG_CHECK_MODULES(SYSTEMD_LOGIN,
                    [libsystemd >= 209],
                    [have_systemd_login_209=yes;have_systemd_login=yes],
                    [have_systemd_login_209=no;have_systemd_login=no])

  # Older versions of systemd package systemd-login separately. Fall back on that
  AS_IF([test x$have_systemd_login != xyes],[
        PKG_CHECK_MODULES(SYSTEMD_LOGIN,
                          [libsystemd-login >= 198],
                          [have_systemd_login=yes],
                          [have_systemd_login=no])
        ])
else
  have_systemd_login=no
fi

if test "x$have_systemd_login" = "xno" -a "x$enable_systemd_login" = "xyes"; then
  AC_MSG_ERROR([systemd-login support explicitly enabled, but can't find libsystemd>=209, libsystemd-login or dbus])
fi

AS_IF([test "x$have_systemd_login" = "xyes"],
      [AC_DEFINE([HAVE_SYSTEMD_LOGIN], [1], [Have systemd-login])])
AM_CONDITIONAL(HAVE_SYSTEMD_LOGIN, test "x$have_systemd_login" = "xyes")

AS_IF([test "x$have_systemd_login_209" = "xyes"],
      [AC_DEFINE([HAVE_SYSTEMD_LOGIN_209], [1], [Have systemd-login >= 209])])


# Note that other features might want libxml2, or this feature might use
# alternative xml libraries at some point. Therefore the feature and
# pre-requisite concepts are split.
AC_ARG_ENABLE(junit_xml,
	      AS_HELP_STRING([--disable-junit-xml],
			     [do not build with JUnit XML output]),,
	      enable_junit_xml=auto)
if test "x$enable_junit_xml" != "xno"; then
	PKG_CHECK_MODULES(LIBXML2,
			  [libxml-2.0 >= 2.6],
			  have_libxml2=yes,
			  have_libxml2=no)
	if test "x$have_libxml2" = "xno" -a "x$enable_junit_xml" = "xyes"; then
		AC_MSG_ERROR([JUnit XML support explicitly requested, but libxml2 couldn't be found])
	fi
	if test "x$have_libxml2" = "xyes"; then
		enable_junit_xml=yes
		AC_DEFINE(ENABLE_JUNIT_XML, [1], [Build Weston with JUnit output support])
	else
		enable_junit_xml=no
	fi
fi
AM_CONDITIONAL(ENABLE_JUNIT_XML, test "x$enable_junit_xml" = "xyes")

# ivi-shell support
AC_ARG_ENABLE(ivi-shell,
              AS_HELP_STRING([--disable-ivi-shell],
                             [do not build ivi-shell server plugin and client]),,
	      enable_ivi_shell=yes)
AM_CONDITIONAL(ENABLE_IVI_SHELL, test "x$enable_ivi_shell" = "xyes")

AC_ARG_ENABLE(wcap-tools, [  --disable-wcap-tools],, enable_wcap_tools=yes)
AM_CONDITIONAL(BUILD_WCAP_TOOLS, test x$enable_wcap_tools = xyes)
if test x$enable_wcap_tools = xyes; then
  AC_DEFINE([BUILD_WCAP_TOOLS], [1], [Build the wcap tools])
  PKG_CHECK_MODULES(WCAP, [cairo])
  WCAP_LIBS="$WCAP_LIBS -lm"
fi

PKG_CHECK_MODULES(SETBACKLIGHT, [libudev libdrm], enable_setbacklight=yes, enable_setbacklight=no)
AM_CONDITIONAL(BUILD_SETBACKLIGHT, test "x$enable_setbacklight" = "xyes")

if test "x$GCC" = "xyes"; then
	GCC_CFLAGS="-Wall -Wextra -Wno-unused-parameter \
		-Wno-shift-negative-value -Wno-missing-field-initializers \
		-g -fvisibility=hidden \
		-Wstrict-prototypes -Wmissing-prototypes -Wsign-compare"
fi
AC_SUBST(GCC_CFLAGS)


if test "x$WESTON_NATIVE_BACKEND" = "x"; then
	WESTON_NATIVE_BACKEND="drm-backend.so"
fi
AC_MSG_NOTICE([Weston's native backend: $WESTON_NATIVE_BACKEND])
AC_DEFINE_UNQUOTED([WESTON_NATIVE_BACKEND], ["$WESTON_NATIVE_BACKEND"],
		   [The default backend to load, if not wayland nor x11.])

if test "x$WESTON_SHELL_CLIENT" = "x"; then
	WESTON_SHELL_CLIENT="weston-desktop-shell"
fi
AC_MSG_NOTICE([Weston's default desktop shell client: $WESTON_SHELL_CLIENT])
AC_DEFINE_UNQUOTED([WESTON_SHELL_CLIENT], ["$WESTON_SHELL_CLIENT"],
		   [The default desktop shell client to load.])

AC_ARG_ENABLE(demo-clients-install,
              AS_HELP_STRING([--enable-demo-clients-install],
                             [Install demo clients built with weston]),,
			     enable_demo_clients_install=no)
AM_CONDITIONAL(INSTALL_DEMO_CLIENTS, [test "x$enable_demo_clients_install" = "xyes"])

AC_ARG_ENABLE(lcms,
              AS_HELP_STRING([--disable-lcms],
                             [Disable lcms support]),,
              enable_lcms=auto)
have_lcms=no
if test "x$enable_lcms" != "xno"; then
        PKG_CHECK_MODULES(LCMS,
                          lcms2,
                          have_lcms=yes,
                          have_lcms=no)
        if test "x$have_lcms" = "xno" -a "x$enable_lcms" = "xyes"; then
          AC_MSG_ERROR([lcms support explicitly requested, but lcms couldn't be found])
        fi
        if test "x$have_lcms" = "xyes"; then
                enable_lcms=yes
                AC_DEFINE(HAVE_LCMS, 1, [Have lcms support])
        fi
fi
AM_CONDITIONAL(HAVE_LCMS, [test "x$enable_lcms" = xyes])

AC_PATH_PROG([wayland_scanner], [wayland-scanner])
if test x$wayland_scanner = x; then
	PKG_CHECK_MODULES(WAYLAND_SCANNER, [wayland-scanner])
	wayland_scanner=`$PKG_CONFIG --variable=wayland_scanner wayland-scanner`
fi

AC_ARG_ENABLE(systemd_notify,
              AS_HELP_STRING([--enable-systemd-notify],
                             [Enables systemd notifications to
                              notify systemd about weston state
                              and update watchdog.
                              Also sockets provided by systemd
                              in case of socket-base activation
                              are added to wayland display]),,
              enable_systemd_notify=no)
AM_CONDITIONAL(SYSTEMD_NOTIFY_SUPPORT, test x$enable_systemd_notify = xyes)
if test "x$enable_systemd_notify" = "xyes"; then
  AC_DEFINE([SYSTEMD_NOTIFY_SUPPORT], [1], [Build the systemd sd_notify support])
  PKG_CHECK_MODULES(SYSTEMD_DAEMON, [libsystemd])
fi

AC_CONFIG_FILES([Makefile libweston/version.h compositor/weston.pc])

# AC_CONFIG_FILES needs the full name when running autoconf, so we need to use
# libweston_abi_version here, and outside [] because of m4 quoting rules
AC_CONFIG_FILES([libweston/libweston-]libweston_major_version[.pc:libweston/libweston.pc.in])
AC_CONFIG_FILES([libweston/libweston-]libweston_major_version[-uninstalled.pc:libweston/libweston-uninstalled.pc.in])
AC_CONFIG_FILES([libweston-desktop/libweston-desktop-]libweston_major_version[.pc:libweston-desktop/libweston-desktop.pc.in])
AC_CONFIG_FILES([libweston-desktop/libweston-desktop-]libweston_major_version[-uninstalled.pc:libweston-desktop/libweston-desktop-uninstalled.pc.in])

AM_CONDITIONAL([HAVE_GIT_REPO], [test -f $srcdir/.git/logs/HEAD])

AC_OUTPUT

AC_MSG_RESULT([
	Native Backend			${WESTON_NATIVE_BACKEND}
	setuid Install			${enable_setuid_install}

	Cairo Renderer			${with_cairo}
	EGL				${enable_egl}
	xcb_xkb				${have_xcb_xkb}
	XWayland			${enable_xwayland}
	dbus				${enable_dbus}

	ivi-shell			${enable_ivi_shell}

	Build wcap utility		${enable_wcap_tools}
	Build Fullscreen Shell		${enable_fullscreen_shell}
	Enable developer documentation	${enable_devdocs}

	weston-launch utility		${enable_weston_launch}
	systemd-login support		${have_systemd_login}
	systemd notify support		${enable_systemd_notify}

	DRM Compositor			${enable_drm_compositor}
	X11 Compositor			${enable_x11_compositor}
	Wayland Compositor		${enable_wayland_compositor}
	Headless Compositor		${enable_headless_compositor}
	FBDEV Compositor		${enable_fbdev_compositor}
	RDP Compositor			${enable_rdp_compositor}
	Screen Sharing			${enable_screen_sharing}
	JUnit XML output		${enable_junit_xml}

	Build Clients			${enable_clients}
	Build EGL Clients		${have_cairo_egl}
	Build Simple Clients		${enable_simple_clients}
	Build Simple EGL Clients	${enable_simple_egl_clients}

	Install Demo Clients		${enable_demo_clients_install}

	Colord Support			${have_colord}
	LCMS2 Support			${have_lcms}
	libjpeg Support			${have_jpeglib}
	libwebp Support			${have_webp}
	VA H.264 encoding Support	${have_libva}
])
