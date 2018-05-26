ACLOCAL_AMFLAGS = -I m4

bin_PROGRAMS =
noinst_PROGRAMS =
libexec_PROGRAMS =
moduledir = $(libdir)/weston
module_LTLIBRARIES =
libweston_moduledir = $(libdir)/libweston-$(LIBWESTON_MAJOR)
libweston_module_LTLIBRARIES =
noinst_LTLIBRARIES =
BUILT_SOURCES =

AM_DISTCHECK_CONFIGURE_FLAGS = --disable-setuid-install

EXTRA_DIST = weston.ini.in ivi-shell/weston.ini.in

weston.ini : $(srcdir)/weston.ini.in
	$(AM_V_GEN)$(SED) \
		-e 's|@bindir[@]|$(bindir)|g' \
		-e 's|@abs_top_builddir[@]|$(abs_top_builddir)|g' \
		-e 's|@libexecdir[@]|$(libexecdir)|g' \
		$< > $@

ivi-shell/weston.ini : $(srcdir)/ivi-shell/weston.ini.in
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(SED) \
		-e 's|@bindir[@]|$(bindir)|g' \
		-e 's|@abs_top_builddir[@]|$(abs_top_builddir)|g' \
		-e 's|@abs_top_srcdir[@]|$(abs_top_srcdir)|g' \
		-e 's|@libexecdir[@]|$(libexecdir)|g' \
		-e 's|@plugin_prefix[@]||g' \
		$< > $@

all-local : weston.ini ivi-shell/weston.ini

AM_CFLAGS = $(GCC_CFLAGS)

AM_CPPFLAGS = 					\
	-I$(top_builddir)/libweston		\
	-I$(top_srcdir)/libweston		\
	-I$(top_builddir)/clients		\
	-I$(top_builddir)/tests			\
	-I$(top_srcdir)/shared			\
	-I$(top_builddir)/protocol		\
	-DLIBWESTON_MODULEDIR='"$(libweston_moduledir)"' \
	-DLIBEXECDIR='"$(libexecdir)"'		\
	-DBINDIR='"$(bindir)"'

CLEANFILES = weston.ini				\
	ivi-shell/weston.ini			\
	internal-screenshot-00.png		\
	$(BUILT_SOURCES)

# Libtool race fix
# libweston-desktop depends on libweston, and desktop-shell depends on both.
# This leads to a libtool race at installation, because libtool re-links
# everything.
# If you add more fixes, you may need a workaround to keep automake generated
# targets. See http://debbugs.gnu.org/cgi/bugreport.cgi?bug=7328
install-libweston_moduleLTLIBRARIES install-moduleLTLIBRARIES: install-libLTLIBRARIES

lib_LTLIBRARIES = libweston-@LIBWESTON_MAJOR@.la
libweston_@LIBWESTON_MAJOR@_la_CPPFLAGS = $(AM_CPPFLAGS) -DIN_WESTON
libweston_@LIBWESTON_MAJOR@_la_CFLAGS = $(AM_CFLAGS) \
	$(COMPOSITOR_CFLAGS) $(EGL_CFLAGS) $(LIBDRM_CFLAGS)
libweston_@LIBWESTON_MAJOR@_la_LIBADD = $(COMPOSITOR_LIBS) \
	$(DL_LIBS) -lm $(CLOCK_GETTIME_LIBS) \
	$(LIBINPUT_BACKEND_LIBS) libshared.la
libweston_@LIBWESTON_MAJOR@_la_LDFLAGS = -version-info $(LT_VERSION_INFO)

libweston_@LIBWESTON_MAJOR@_la_SOURCES =			\
	libweston/git-version.h				\
	libweston/log.c					\
	libweston/compositor.c				\
	libweston/compositor.h				\
	libweston/compositor-drm.h			\
	libweston/compositor-fbdev.h			\
	libweston/compositor-headless.h			\
	libweston/compositor-rdp.h			\
	libweston/compositor-wayland.h			\
	libweston/compositor-x11.h			\
	libweston/input.c				\
	libweston/data-device.c				\
	libweston/screenshooter.c			\
	libweston/clipboard.c				\
	libweston/zoom.c				\
	libweston/bindings.c				\
	libweston/animation.c				\
	libweston/noop-renderer.c			\
	libweston/pixman-renderer.c			\
	libweston/pixman-renderer.h			\
	libweston/plugin-registry.c				\
	libweston/plugin-registry.h				\
	libweston/timeline.c				\
	libweston/timeline.h				\
	libweston/timeline-object.h			\
	libweston/linux-dmabuf.c			\
	libweston/linux-dmabuf.h			\
	libweston/pixel-formats.c			\
	libweston/pixel-formats.h			\
	shared/helpers.h				\
	shared/matrix.c					\
	shared/matrix.h					\
	shared/timespec-util.h				\
	shared/zalloc.h					\
	shared/platform.h				\
	shared/weston-egl-ext.h

lib_LTLIBRARIES += libweston-desktop-@LIBWESTON_MAJOR@.la
libweston_desktop_@LIBWESTON_MAJOR@_la_CPPFLAGS = $(AM_CPPFLAGS) -DIN_WESTON
libweston_desktop_@LIBWESTON_MAJOR@_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
libweston_desktop_@LIBWESTON_MAJOR@_la_LIBADD =		\
	libweston-@LIBWESTON_MAJOR@.la			\
	$(COMPOSITOR_LIBS)
libweston_desktop_@LIBWESTON_MAJOR@_la_LDFLAGS = -version-info $(LT_VERSION_INFO)

libweston_desktop_@LIBWESTON_MAJOR@_la_SOURCES =	\
	libweston-desktop/client.c			\
	libweston-desktop/internal.h			\
	libweston-desktop/libweston-desktop.c		\
	libweston-desktop/libweston-desktop.h		\
	libweston-desktop/seat.c			\
	libweston-desktop/surface.c			\
	libweston-desktop/wl-shell.c			\
	libweston-desktop/xdg-shell-v6.c		\
	libweston-desktop/xwayland.c

nodist_libweston_desktop_@LIBWESTON_MAJOR@_la_SOURCES =		\
	protocol/xdg-shell-unstable-v6-protocol.c		\
	protocol/xdg-shell-unstable-v6-server-protocol.h

BUILT_SOURCES += $(nodist_libweston_desktop_@LIBWESTON_MAJOR@_la_SOURCES)

libweston-desktop-@LIBWESTON_MAJOR@.la libweston-desktop/libweston_desktop_@LIBWESTON_MAJOR@_la-xdg-shell-v6.lo: protocol/xdg-shell-unstable-v6-server-protocol.h

if SYSTEMD_NOTIFY_SUPPORT
module_LTLIBRARIES += systemd-notify.la
systemd_notify_la_LDFLAGS = -module -avoid-version
systemd_notify_la_LIBADD = libweston-@LIBWESTON_MAJOR@.la $(SYSTEMD_DAEMON_LIBS)
systemd_notify_la_CFLAGS =			\
	$(COMPOSITOR_CFLAGS)			\
	$(SYSTEMD_DAEMON_CFLAGS)		\
	$(PIXMAN_CFLAGS)			\
	$(AM_CFLAGS)
systemd_notify_la_SOURCES =			\
	compositor/systemd-notify.c		\
	shared/helpers.h			\
	shared/zalloc.h				\
	libweston/compositor.h
endif

nodist_libweston_@LIBWESTON_MAJOR@_la_SOURCES =				\
	protocol/weston-screenshooter-protocol.c			\
	protocol/weston-screenshooter-server-protocol.h			\
	protocol/text-cursor-position-protocol.c	\
	protocol/text-cursor-position-server-protocol.h	\
	protocol/text-input-unstable-v1-protocol.c			\
	protocol/text-input-unstable-v1-server-protocol.h		\
	protocol/input-method-unstable-v1-protocol.c			\
	protocol/input-method-unstable-v1-server-protocol.h		\
	protocol/presentation-time-protocol.c		\
	protocol/presentation-time-server-protocol.h	\
	protocol/viewporter-protocol.c			\
	protocol/viewporter-server-protocol.h		\
	protocol/linux-dmabuf-unstable-v1-protocol.c	\
	protocol/linux-dmabuf-unstable-v1-server-protocol.h		\
	protocol/relative-pointer-unstable-v1-protocol.c		\
	protocol/relative-pointer-unstable-v1-server-protocol.h		\
	protocol/pointer-constraints-unstable-v1-protocol.c		\
	protocol/pointer-constraints-unstable-v1-server-protocol.h      \
	protocol/input-timestamps-unstable-v1-protocol.c		\
	protocol/input-timestamps-unstable-v1-server-protocol.h

BUILT_SOURCES += $(nodist_libweston_@LIBWESTON_MAJOR@_la_SOURCES)

bin_PROGRAMS += weston

weston_LDFLAGS = -export-dynamic
weston_CPPFLAGS = $(AM_CPPFLAGS) -DIN_WESTON 		\
				 -DMODULEDIR='"$(moduledir)"' \
				 -DXSERVER_PATH='"@XSERVER_PATH@"'
weston_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS) $(LIBINPUT_BACKEND_CFLAGS)
weston_LDADD = libshared.la libweston-@LIBWESTON_MAJOR@.la \
	$(COMPOSITOR_LIBS) \
	$(DL_LIBS) $(LIBINPUT_BACKEND_LIBS) \
	$(CLOCK_GETRES_LIBS) \
	-lm -lpthread

weston_SOURCES = 					\
	compositor/main.c				\
	compositor/weston-screenshooter.c		\
	compositor/text-backend.c			\
	compositor/xwayland.c

# Track this dependency explicitly instead of using BUILT_SOURCES.  We
# add BUILT_SOURCES to CLEANFILES, but we want to keep git-version.h
# in case we're building from tarballs.

compositor/main.c : $(top_builddir)/libweston/git-version.h
libweston/compositor.c : $(top_builddir)/libweston/git-version.h

noinst_LTLIBRARIES +=				\
	libsession-helper.la

libsession_helper_la_SOURCES =			\
	libweston/launcher-util.c		\
	libweston/launcher-util.h		\
	libweston/launcher-impl.h		\
	libweston/weston-launch.h		\
	libweston/launcher-weston-launch.c	\
	libweston/launcher-direct.c
libsession_helper_la_CFLAGS = $(AM_CFLAGS) $(LIBDRM_CFLAGS) $(PIXMAN_CFLAGS) $(COMPOSITOR_CFLAGS)
libsession_helper_la_LIBADD = libweston-@LIBWESTON_MAJOR@.la

if ENABLE_DRM_COMPOSITOR
libsession_helper_la_LIBADD += $(LIBDRM_LIBS)
endif

if ENABLE_DBUS
if HAVE_SYSTEMD_LOGIN
libsession_helper_la_SOURCES +=			\
	libweston/dbus.h			\
	libweston/dbus.c			\
	libweston/launcher-logind.c
libsession_helper_la_CFLAGS += $(SYSTEMD_LOGIN_CFLAGS) $(DBUS_CFLAGS)
libsession_helper_la_LIBADD += $(SYSTEMD_LOGIN_LIBS) $(DBUS_LIBS)
endif
endif

if HAVE_GIT_REPO
libweston/git-version.h : $(top_srcdir)/.git/logs/HEAD
	$(AM_V_GEN)echo "#define BUILD_ID \"$(shell git --git-dir=$(top_srcdir)/.git describe --always --dirty) $(shell git --git-dir=$(top_srcdir)/.git log -1 --format='%s (%ci)')\"" > $@
else
libweston/git-version.h :
	$(AM_V_GEN)echo "#define BUILD_ID \"unknown (not built from git or tarball)\"" > $@

endif

.FORCE :

if BUILD_WESTON_LAUNCH
bin_PROGRAMS += weston-launch
weston_launch_SOURCES = libweston/weston-launch.c libweston/weston-launch.h
weston_launch_CPPFLAGS = -DBINDIR='"$(bindir)"'
weston_launch_CFLAGS=				\
	$(AM_CFLAGS)				\
	$(PAM_CFLAGS)				\
	$(SYSTEMD_LOGIN_CFLAGS)			\
	$(LIBDRM_CFLAGS)
weston_launch_LDADD = $(PAM_LIBS) $(SYSTEMD_LOGIN_LIBS)

if ENABLE_DRM_COMPOSITOR
weston_launch_LDADD += $(LIBDRM_LIBS)
endif

if ENABLE_SETUID_INSTALL
install-exec-hook:
	can_suid_files=no; \
	chown root $(DESTDIR)$(bindir)/weston-launch \
		&& chmod u+s $(DESTDIR)$(bindir)/weston-launch \
		&& can_suid_files=yes;\
	if test $$can_suid_files = no; then \
		echo 'Error: unable to unable to change ownership/setuid on weston-launch.'; \
		echo 'To skip this step, re-run ./configure using --disable-setuid-install'; \
		false; \
	fi
endif

endif # BUILD_WESTON_LAUNCH

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = \
	libweston/libweston-${LIBWESTON_MAJOR}.pc \
	libweston-desktop/libweston-desktop-${LIBWESTON_MAJOR}.pc \
	compositor/weston.pc

wayland_sessiondir = $(datadir)/wayland-sessions
dist_wayland_session_DATA = compositor/weston.desktop

libwestonincludedir = $(includedir)/libweston-${LIBWESTON_MAJOR}
libwestoninclude_HEADERS =			\
	libweston/version.h			\
	libweston/compositor.h			\
	libweston/compositor-drm.h		\
	libweston/compositor-fbdev.h		\
	libweston/compositor-headless.h		\
	libweston/compositor-rdp.h		\
	libweston/compositor-wayland.h		\
	libweston/compositor-x11.h		\
	libweston/windowed-output-api.h		\
	libweston/plugin-registry.h		\
	libweston/timeline-object.h		\
	shared/matrix.h				\
	shared/config-parser.h			\
	shared/zalloc.h

libwestoninclude_HEADERS +=			\
	libweston-desktop/libweston-desktop.h

westonincludedir = $(includedir)/weston
westoninclude_HEADERS = compositor/weston.h

if ENABLE_IVI_SHELL
westoninclude_HEADERS +=				\
	ivi-shell/ivi-layout-export.h
endif

if ENABLE_EGL
libweston_module_LTLIBRARIES += gl-renderer.la
gl_renderer_la_LDFLAGS = -module -avoid-version
gl_renderer_la_LIBADD =				\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(EGL_LIBS)				\
	$(COMPOSITOR_LIBS)
gl_renderer_la_CFLAGS =				\
	$(COMPOSITOR_CFLAGS)			\
	$(EGL_CFLAGS)				\
	$(LIBDRM_CFLAGS)			\
	$(AM_CFLAGS)
gl_renderer_la_SOURCES =			\
	libweston/gl-renderer.h			\
	libweston/gl-renderer.c			\
	libweston/vertex-clipping.c		\
	libweston/vertex-clipping.h		\
	libweston/weston-sync-file.h		\
	shared/helpers.h
endif

if ENABLE_X11_COMPOSITOR
libweston_module_LTLIBRARIES += x11-backend.la
x11_backend_la_LDFLAGS = -module -avoid-version
x11_backend_la_LIBADD =				\
	libshared-cairo.la			\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(X11_COMPOSITOR_LIBS)			\
	$(COMPOSITOR_LIBS)
x11_backend_la_CFLAGS =				\
	$(AM_CFLAGS)				\
	$(COMPOSITOR_CFLAGS)			\
	$(EGL_CFLAGS)				\
	$(PIXMAN_CFLAGS)			\
	$(CAIRO_CFLAGS)				\
	$(X11_COMPOSITOR_CFLAGS)
x11_backend_la_SOURCES = 			\
	libweston/compositor-x11.c		\
	libweston/compositor-x11.h		\
	shared/helpers.h
endif

INPUT_BACKEND_CFLAGS = $(LIBINPUT_BACKEND_CFLAGS)
INPUT_BACKEND_LIBS = $(LIBINPUT_BACKEND_LIBS)
INPUT_BACKEND_SOURCES =				\
	libweston/libinput-seat.c		\
	libweston/libinput-seat.h		\
	libweston/libinput-device.c		\
	libweston/libinput-device.h		\
	shared/helpers.h

if ENABLE_DRM_COMPOSITOR
libweston_module_LTLIBRARIES += drm-backend.la
drm_backend_la_LDFLAGS = -module -avoid-version
drm_backend_la_LIBADD =				\
	libsession-helper.la			\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(COMPOSITOR_LIBS)			\
	$(DRM_COMPOSITOR_LIBS)			\
	$(INPUT_BACKEND_LIBS)			\
	libshared.la				\
	$(CLOCK_GETTIME_LIBS)
drm_backend_la_CFLAGS =				\
	$(COMPOSITOR_CFLAGS)			\
	$(EGL_CFLAGS)				\
	$(DRM_COMPOSITOR_CFLAGS)		\
	$(INPUT_BACKEND_CFLAGS)			\
	$(AM_CFLAGS)
drm_backend_la_SOURCES =			\
	libweston/compositor-drm.c		\
	libweston/compositor-drm.h		\
	$(INPUT_BACKEND_SOURCES)		\
	shared/helpers.h			\
	shared/timespec-util.h			\
	libweston/libbacklight.c		\
	libweston/libbacklight.h

if ENABLE_VAAPI_RECORDER
drm_backend_la_SOURCES += libweston/vaapi-recorder.c libweston/vaapi-recorder.h
drm_backend_la_LIBADD += $(LIBVA_LIBS)
drm_backend_la_LDFLAGS += -pthread
drm_backend_la_CFLAGS += $(LIBVA_CFLAGS)
endif
endif

if ENABLE_WAYLAND_COMPOSITOR
libweston_module_LTLIBRARIES += wayland-backend.la
wayland_backend_la_LDFLAGS = -module -avoid-version
wayland_backend_la_LIBADD =			\
	libshared-cairo.la			\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(COMPOSITOR_LIBS)			\
	$(WAYLAND_COMPOSITOR_EGL_LIBS)		\
	$(WAYLAND_COMPOSITOR_LIBS)
wayland_backend_la_CFLAGS =			\
	$(COMPOSITOR_CFLAGS)			\
	$(EGL_CFLAGS)				\
	$(PIXMAN_CFLAGS)			\
	$(CAIRO_CFLAGS)				\
	$(WAYLAND_COMPOSITOR_CFLAGS)		\
	$(AM_CFLAGS)
wayland_backend_la_SOURCES = 					\
	libweston/compositor-wayland.c				\
	libweston/compositor-wayland.h				\
	shared/helpers.h
nodist_wayland_backend_la_SOURCES =				\
	protocol/fullscreen-shell-unstable-v1-protocol.c	\
	protocol/fullscreen-shell-unstable-v1-client-protocol.h	\
	protocol/xdg-shell-unstable-v6-protocol.c		\
	protocol/xdg-shell-unstable-v6-client-protocol.h
endif

if ENABLE_HEADLESS_COMPOSITOR
libweston_module_LTLIBRARIES += headless-backend.la
headless_backend_la_LDFLAGS = -module -avoid-version
headless_backend_la_LIBADD =			\
	libshared.la				\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(COMPOSITOR_LIBS)
headless_backend_la_CFLAGS = $(COMPOSITOR_CFLAGS) $(AM_CFLAGS)
headless_backend_la_SOURCES = 			\
	libweston/compositor-headless.c		\
	libweston/compositor-headless.h		\
	shared/helpers.h
endif

if ENABLE_FBDEV_COMPOSITOR
libweston_module_LTLIBRARIES += fbdev-backend.la
fbdev_backend_la_LDFLAGS = -module -avoid-version
fbdev_backend_la_LIBADD =			\
	libshared.la				\
	libsession-helper.la			\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(COMPOSITOR_LIBS)			\
	$(FBDEV_COMPOSITOR_LIBS)		\
	$(INPUT_BACKEND_LIBS)
fbdev_backend_la_CFLAGS =			\
	$(COMPOSITOR_CFLAGS)			\
	$(EGL_CFLAGS)				\
	$(FBDEV_COMPOSITOR_CFLAGS)		\
	$(PIXMAN_CFLAGS)			\
	$(INPUT_BACKEND_CFLAGS)			\
	$(AM_CFLAGS)
fbdev_backend_la_SOURCES =			\
	libweston/compositor-fbdev.c		\
	libweston/compositor-fbdev.h		\
	shared/helpers.h			\
	$(INPUT_BACKEND_SOURCES)
endif

if ENABLE_RDP_COMPOSITOR
libweston_module_LTLIBRARIES += rdp-backend.la
rdp_backend_la_LDFLAGS = -module -avoid-version
rdp_backend_la_LIBADD =				\
	libshared.la				\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(COMPOSITOR_LIBS)		\
	$(RDP_COMPOSITOR_LIBS)
rdp_backend_la_CFLAGS =				\
	$(COMPOSITOR_CFLAGS)			\
	$(RDP_COMPOSITOR_CFLAGS)		\
	$(AM_CFLAGS)
rdp_backend_la_SOURCES = 			\
	libweston/compositor-rdp.c		\
	libweston/compositor-rdp.h		\
	shared/helpers.h
endif

if HAVE_LCMS
module_LTLIBRARIES += cms-static.la
cms_static_la_LDFLAGS = -module -avoid-version
cms_static_la_LIBADD =				\
	libshared.la				\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(LCMS_LIBS)				\
	$(COMPOSITOR_LIBS)
cms_static_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS) $(LCMS_CFLAGS)
cms_static_la_SOURCES =				\
	compositor/cms-static.c			\
	compositor/cms-helper.c			\
	compositor/cms-helper.h			\
	shared/helpers.h
if ENABLE_COLORD
module_LTLIBRARIES += cms-colord.la
cms_colord_la_LDFLAGS = -module -avoid-version
cms_colord_la_LIBADD =				\
	libshared.la				\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(COLORD_LIBS)				\
	$(LCMS_LIBS)				\
	$(COMPOSITOR_LIBS)
cms_colord_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS) $(COLORD_CFLAGS)
cms_colord_la_SOURCES =				\
	compositor/cms-colord.c			\
	compositor/cms-helper.c			\
	compositor/cms-helper.h			\
	shared/helpers.h
endif
endif

noinst_PROGRAMS += spring-tool
spring_tool_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
spring_tool_LDADD = $(COMPOSITOR_LIBS) -lm
spring_tool_SOURCES =				\
	libweston/spring-tool.c			\
	libweston/animation.c			\
	shared/matrix.c				\
	shared/matrix.h				\
	libweston/compositor.h

if BUILD_CLIENTS

bin_PROGRAMS += weston-terminal weston-info

libexec_PROGRAMS +=				\
	weston-desktop-shell			\
	weston-screenshooter			\
	weston-keyboard				\
	weston-simple-im

if ENABLE_IVI_SHELL
libexec_PROGRAMS +=				\
	weston-ivi-shell-user-interface
endif

demo_clients =					\
	weston-flower				\
	weston-image				\
	weston-cliptest				\
	weston-dnd				\
	weston-smoke				\
	weston-resizor				\
	weston-eventdemo			\
	weston-clickdot				\
	weston-confine				\
	weston-transformed			\
	weston-fullscreen			\
	weston-stacking				\
	weston-calibrator			\
	weston-scaler

if INSTALL_DEMO_CLIENTS
bin_PROGRAMS += $(demo_clients)
else
noinst_PROGRAMS += $(demo_clients)
endif


if BUILD_SIMPLE_CLIENTS
demo_clients +=					\
	weston-simple-shm			\
	weston-simple-damage			\
	weston-simple-touch			\
	weston-presentation-shm			\
	weston-multi-resource

weston_simple_shm_SOURCES = clients/simple-shm.c
nodist_weston_simple_shm_SOURCES =		\
	protocol/xdg-shell-unstable-v6-protocol.c		\
	protocol/xdg-shell-unstable-v6-client-protocol.h	\
	protocol/fullscreen-shell-unstable-v1-protocol.c	\
	protocol/fullscreen-shell-unstable-v1-client-protocol.h	\
	protocol/ivi-application-protocol.c		\
	protocol/ivi-application-client-protocol.h
weston_simple_shm_CFLAGS = $(AM_CFLAGS) $(SIMPLE_CLIENT_CFLAGS)
weston_simple_shm_LDADD = $(SIMPLE_CLIENT_LIBS) libshared.la

weston_simple_damage_SOURCES = clients/simple-damage.c
nodist_weston_simple_damage_SOURCES =		\
	protocol/viewporter-protocol.c		\
	protocol/viewporter-client-protocol.h	\
	protocol/xdg-shell-unstable-v6-protocol.c		\
	protocol/xdg-shell-unstable-v6-client-protocol.h	\
	protocol/fullscreen-shell-unstable-v1-protocol.c	\
	protocol/fullscreen-shell-unstable-v1-client-protocol.h
weston_simple_damage_CFLAGS = $(AM_CFLAGS) $(SIMPLE_CLIENT_CFLAGS)
weston_simple_damage_LDADD = $(SIMPLE_CLIENT_LIBS) libshared.la

weston_simple_touch_SOURCES = 			\
	clients/simple-touch.c			\
	shared/helpers.h
weston_simple_touch_CFLAGS = $(AM_CFLAGS) $(SIMPLE_CLIENT_CFLAGS)
weston_simple_touch_LDADD = $(SIMPLE_CLIENT_LIBS) libshared.la

weston_presentation_shm_SOURCES = 			\
	clients/presentation-shm.c			\
	shared/helpers.h
nodist_weston_presentation_shm_SOURCES =		\
	protocol/presentation-time-protocol.c		\
	protocol/presentation-time-client-protocol.h
weston_presentation_shm_CFLAGS = $(AM_CFLAGS) $(SIMPLE_CLIENT_CFLAGS)
weston_presentation_shm_LDADD = $(SIMPLE_CLIENT_LIBS) libshared.la -lm $(CLOCK_GETTIME_LIBS)

weston_multi_resource_SOURCES = clients/multi-resource.c
weston_multi_resource_CFLAGS = $(AM_CFLAGS) $(SIMPLE_CLIENT_CFLAGS)
weston_multi_resource_LDADD = $(SIMPLE_CLIENT_LIBS) libshared.la $(CLOCK_GETTIME_LIBS) -lm
endif

if BUILD_SIMPLE_EGL_CLIENTS
demo_clients += weston-simple-egl
weston_simple_egl_SOURCES = clients/simple-egl.c
nodist_weston_simple_egl_SOURCES =		\
	protocol/xdg-shell-unstable-v6-protocol.c		\
	protocol/xdg-shell-unstable-v6-client-protocol.h	\
	protocol/ivi-application-protocol.c		\
	protocol/ivi-application-client-protocol.h
weston_simple_egl_CFLAGS = $(AM_CFLAGS) $(SIMPLE_EGL_CLIENT_CFLAGS)
weston_simple_egl_LDADD = $(SIMPLE_EGL_CLIENT_LIBS) -lm
endif

if BUILD_SIMPLE_DMABUF_DRM_CLIENT
demo_clients += weston-simple-dmabuf-drm
weston_simple_dmabuf_drm_SOURCES =			\
	clients/simple-dmabuf-drm.c			\
	clients/simple-dmabuf-drm-data.h
nodist_weston_simple_dmabuf_drm_SOURCES =		\
	protocol/xdg-shell-unstable-v6-protocol.c		\
	protocol/xdg-shell-unstable-v6-client-protocol.h	\
	protocol/fullscreen-shell-unstable-v1-protocol.c	\
	protocol/fullscreen-shell-unstable-v1-client-protocol.h \
	protocol/linux-dmabuf-unstable-v1-protocol.c \
	protocol/linux-dmabuf-unstable-v1-client-protocol.h
weston_simple_dmabuf_drm_CFLAGS = $(AM_CFLAGS) $(SIMPLE_DMABUF_DRM_CLIENT_CFLAGS)
weston_simple_dmabuf_drm_LDADD = $(SIMPLE_DMABUF_DRM_CLIENT_LIBS) \
	$(LIBDRM_PLATFORM_FREEDRENO_LIBS) \
	$(LIBDRM_PLATFORM_INTEL_LIBS)     \
	libshared.la
endif

if BUILD_SIMPLE_DMABUF_V4L_CLIENT
demo_clients += weston-simple-dmabuf-v4l
weston_simple_dmabuf_v4l_SOURCES = clients/simple-dmabuf-v4l.c
nodist_weston_simple_dmabuf_v4l_SOURCES =		\
	protocol/xdg-shell-unstable-v6-protocol.c		\
	protocol/xdg-shell-unstable-v6-client-protocol.h	\
	protocol/fullscreen-shell-unstable-v1-protocol.c	\
	protocol/fullscreen-shell-unstable-v1-client-protocol.h \
	protocol/linux-dmabuf-unstable-v1-protocol.c \
	protocol/linux-dmabuf-unstable-v1-client-protocol.h
weston_simple_dmabuf_v4l_CFLAGS = $(AM_CFLAGS) $(SIMPLE_DMABUF_V4L_CLIENT_CFLAGS)
weston_simple_dmabuf_v4l_LDADD = $(SIMPLE_DMABUF_V4L_CLIENT_LIBS) libshared.la
endif

noinst_LTLIBRARIES += libtoytoolkit.la

libtoytoolkit_la_SOURCES =				\
	clients/window.c				\
	clients/window.h				\
	shared/helpers.h

nodist_libtoytoolkit_la_SOURCES =			\
	protocol/text-cursor-position-protocol.c	\
	protocol/text-cursor-position-client-protocol.h	\
	protocol/viewporter-protocol.c			\
	protocol/viewporter-client-protocol.h		\
	protocol/xdg-shell-unstable-v6-protocol.c			\
	protocol/xdg-shell-unstable-v6-client-protocol.h		\
	protocol/ivi-application-protocol.c		\
	protocol/ivi-application-client-protocol.h	\
	protocol/pointer-constraints-unstable-v1-protocol.c		\
	protocol/pointer-constraints-unstable-v1-client-protocol.h	\
	protocol/relative-pointer-unstable-v1-protocol.c		\
	protocol/relative-pointer-unstable-v1-client-protocol.h

BUILT_SOURCES += $(nodist_libtoytoolkit_la_SOURCES)


libtoytoolkit_la_LIBADD =			\
	$(CLIENT_LIBS)				\
	$(CAIRO_EGL_LIBS)			\
	libshared-cairo.la $(CLOCK_GETTIME_LIBS) -lm
libtoytoolkit_la_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS) $(CAIRO_EGL_CFLAGS)

weston_flower_SOURCES = clients/flower.c
weston_flower_LDADD = libtoytoolkit.la
weston_flower_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_screenshooter_SOURCES =				\
	clients/screenshot.c
nodist_weston_screenshooter_SOURCES =			\
	protocol/weston-screenshooter-protocol.c	\
	protocol/weston-screenshooter-client-protocol.h
weston_screenshooter_LDADD = $(CLIENT_LIBS) libshared.la
weston_screenshooter_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_terminal_SOURCES = 				\
	clients/terminal.c				\
	shared/helpers.h
weston_terminal_LDADD = libtoytoolkit.la -lutil
weston_terminal_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_image_SOURCES = clients/image.c
weston_image_LDADD = libtoytoolkit.la
weston_image_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_cliptest_SOURCES =				\
	clients/cliptest.c				\
	libweston/vertex-clipping.c			\
	libweston/vertex-clipping.h
weston_cliptest_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)
weston_cliptest_LDADD = libtoytoolkit.la

weston_dnd_SOURCES = 					\
	clients/dnd.c					\
	shared/helpers.h
weston_dnd_LDADD = libtoytoolkit.la
weston_dnd_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_smoke_SOURCES = clients/smoke.c
weston_smoke_LDADD = libtoytoolkit.la
weston_smoke_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_resizor_SOURCES = clients/resizor.c
weston_resizor_LDADD = libtoytoolkit.la
weston_resizor_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_scaler_SOURCES = clients/scaler.c
weston_scaler_LDADD = libtoytoolkit.la
weston_scaler_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

if HAVE_CAIRO_GLESV2
demo_clients += weston-nested weston-nested-client

weston_nested_SOURCES = 				\
	clients/nested.c				\
	shared/helpers.h
weston_nested_LDADD = libtoytoolkit.la $(SERVER_LIBS)
weston_nested_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_nested_client_SOURCES = clients/nested-client.c
weston_nested_client_LDADD = $(SIMPLE_EGL_CLIENT_LIBS) -lm
weston_nested_client_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)
endif

weston_eventdemo_SOURCES = 				\
	clients/eventdemo.c				\
	shared/helpers.h
weston_eventdemo_LDADD = libtoytoolkit.la
weston_eventdemo_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_clickdot_SOURCES = 				\
	clients/clickdot.c				\
	shared/helpers.h
weston_clickdot_LDADD = libtoytoolkit.la
weston_clickdot_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_confine_SOURCES = 				\
	clients/confine.c				\
	shared/helpers.h
weston_confine_LDADD = libtoytoolkit.la
weston_confine_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_transformed_SOURCES = clients/transformed.c
weston_transformed_LDADD = libtoytoolkit.la
weston_transformed_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_fullscreen_SOURCES = clients/fullscreen.c
nodist_weston_fullscreen_SOURCES =			\
	protocol/fullscreen-shell-unstable-v1-protocol.c		\
	protocol/fullscreen-shell-unstable-v1-client-protocol.h
weston_fullscreen_LDADD = libtoytoolkit.la
weston_fullscreen_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_stacking_SOURCES = 				\
	clients/stacking.c				\
	shared/helpers.h
weston_stacking_LDADD = libtoytoolkit.la
weston_stacking_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_calibrator_SOURCES = 				\
	clients/calibrator.c				\
	shared/helpers.h				\
	shared/matrix.c					\
	shared/matrix.h
weston_calibrator_LDADD = libtoytoolkit.la
weston_calibrator_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

if BUILD_SUBSURFACES_CLIENT
demo_clients += weston-subsurfaces
weston_subsurfaces_SOURCES = 			\
	clients/subsurfaces.c			\
	shared/helpers.h
weston_subsurfaces_CFLAGS =			\
	$(AM_CFLAGS)				\
	$(SIMPLE_EGL_CLIENT_CFLAGS)		\
	$(CLIENT_CFLAGS)
weston_subsurfaces_LDADD = libtoytoolkit.la $(SIMPLE_EGL_CLIENT_LIBS) -lm
endif

if HAVE_PANGO
demo_clients += weston-editor
weston_editor_SOURCES = 			\
	clients/editor.c			\
	shared/helpers.h
nodist_weston_editor_SOURCES =			\
	protocol/text-input-unstable-v1-protocol.c		\
	protocol/text-input-unstable-v1-client-protocol.h
weston_editor_LDADD = libtoytoolkit.la $(PANGO_LIBS)
weston_editor_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS) $(PANGO_CFLAGS)
endif

weston_keyboard_SOURCES = clients/keyboard.c
nodist_weston_keyboard_SOURCES =				\
	protocol/weston-desktop-shell-client-protocol.h		\
	protocol/weston-desktop-shell-protocol.c		\
	protocol/input-method-unstable-v1-protocol.c		\
	protocol/input-method-unstable-v1-client-protocol.h
weston_keyboard_LDADD = libtoytoolkit.la
weston_keyboard_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_simple_im_SOURCES = clients/simple-im.c
nodist_weston_simple_im_SOURCES =		\
	protocol/input-method-unstable-v1-protocol.c		\
	protocol/input-method-unstable-v1-client-protocol.h
weston_simple_im_LDADD = $(CLIENT_LIBS)
weston_simple_im_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_info_SOURCES = 					\
	clients/weston-info.c				\
	shared/helpers.h
nodist_weston_info_SOURCES =				\
	protocol/presentation-time-protocol.c		\
	protocol/presentation-time-client-protocol.h	\
	protocol/linux-dmabuf-unstable-v1-protocol.c	\
	protocol/linux-dmabuf-unstable-v1-client-protocol.h
weston_info_LDADD = $(WESTON_INFO_LIBS) libshared.la
weston_info_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

weston_desktop_shell_SOURCES = 				\
	clients/desktop-shell.c				\
	shared/helpers.h
nodist_weston_desktop_shell_SOURCES =			\
	protocol/weston-desktop-shell-client-protocol.h	\
	protocol/weston-desktop-shell-protocol.c
weston_desktop_shell_LDADD = libtoytoolkit.la
weston_desktop_shell_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)

if ENABLE_IVI_SHELL
weston_ivi_shell_user_interface_SOURCES = 				\
	clients/ivi-shell-user-interface.c				\
	shared/helpers.h
nodist_weston_ivi_shell_user_interface_SOURCES =			\
	protocol/ivi-hmi-controller-client-protocol.h			\
	protocol/ivi-hmi-controller-protocol.c				\
	protocol/ivi-application-client-protocol.h			\
        protocol/ivi-application-protocol.c
weston_ivi_shell_user_interface_LDADD = libtoytoolkit.la
weston_ivi_shell_user_interface_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)
endif

if BUILD_FULL_GL_CLIENTS
demo_clients += weston-gears
weston_gears_SOURCES = clients/gears.c
weston_gears_LDADD = libtoytoolkit.la
weston_gears_CFLAGS = $(AM_CFLAGS) $(CLIENT_CFLAGS)
endif

endif

BUILT_SOURCES +=					\
	protocol/weston-screenshooter-protocol.c			\
	protocol/weston-screenshooter-client-protocol.h			\
	protocol/text-cursor-position-client-protocol.h	\
	protocol/text-cursor-position-protocol.c	\
	protocol/text-input-unstable-v1-protocol.c			\
	protocol/text-input-unstable-v1-client-protocol.h		\
	protocol/input-method-unstable-v1-protocol.c			\
	protocol/input-method-unstable-v1-client-protocol.h		\
	protocol/weston-desktop-shell-client-protocol.h			\
	protocol/weston-desktop-shell-protocol.c			\
	protocol/viewporter-client-protocol.h		\
	protocol/viewporter-protocol.c			\
	protocol/presentation-time-protocol.c				\
	protocol/presentation-time-client-protocol.h			\
	protocol/fullscreen-shell-unstable-v1-protocol.c		\
	protocol/fullscreen-shell-unstable-v1-client-protocol.h	\
	protocol/xdg-shell-unstable-v6-protocol.c			\
	protocol/xdg-shell-unstable-v6-client-protocol.h		\
	protocol/ivi-hmi-controller-protocol.c		\
	protocol/ivi-hmi-controller-client-protocol.h	\
	protocol/ivi-application-protocol.c		\
	protocol/ivi-application-client-protocol.h	\
	protocol/linux-dmabuf-unstable-v1-protocol.c	\
	protocol/linux-dmabuf-unstable-v1-client-protocol.h		\
	protocol/input-timestamps-unstable-v1-protocol.c		\
	protocol/input-timestamps-unstable-v1-client-protocol.h

westondatadir = $(datadir)/weston
dist_westondata_DATA =				\
	data/wayland.svg			\
	data/wayland.png			\
	data/pattern.png			\
	data/terminal.png			\
	data/border.png				\
	data/icon_editor.png			\
	data/icon_flower.png			\
	data/icon_terminal.png			\
	data/icon_window.png			\
	data/sign_close.png			\
	data/sign_maximize.png			\
	data/sign_minimize.png

if ENABLE_IVI_SHELL
dist_westondata_DATA +=				\
	data/background.png                     \
	data/tiling.png                         \
	data/fullscreen.png                     \
	data/panel.png                          \
	data/random.png                         \
	data/sidebyside.png                     \
	data/home.png                           \
	data/icon_ivi_clickdot.png              \
	data/icon_ivi_flower.png                \
	data/icon_ivi_simple-egl.png            \
	data/icon_ivi_simple-shm.png            \
	data/icon_ivi_smoke.png
endif


if BUILD_WCAP_TOOLS
bin_PROGRAMS += wcap-decode

wcap_decode_SOURCES =				\
	wcap/main.c				\
	wcap/wcap-decode.c			\
	wcap/wcap-decode.h

wcap_decode_CFLAGS = $(AM_CFLAGS) $(WCAP_CFLAGS)
wcap_decode_LDADD = $(WCAP_LIBS)
endif


if ENABLE_DESKTOP_SHELL

module_LTLIBRARIES += desktop-shell.la

desktop_shell_la_CPPFLAGS =			\
	-I$(top_builddir)/protocol		\
	-I$(top_srcdir)/shared			\
	-I$(top_builddir)/libweston		\
	-I$(top_srcdir)/libweston		\
	-I$(top_builddir)/desktop-shell		\
	-DMODULEDIR='"$(moduledir)"'		\
	-DLIBEXECDIR='"$(libexecdir)"'		\
	-DIN_WESTON

desktop_shell_la_LDFLAGS = -module -avoid-version
desktop_shell_la_LIBADD = libshared.la libweston-desktop-@LIBWESTON_MAJOR@.la $(COMPOSITOR_LIBS)
desktop_shell_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
desktop_shell_la_SOURCES =				\
	desktop-shell/shell.h				\
	desktop-shell/shell.c				\
	desktop-shell/exposay.c				\
	desktop-shell/input-panel.c			\
	shared/helpers.h
nodist_desktop_shell_la_SOURCES =			\
	protocol/weston-desktop-shell-protocol.c	\
	protocol/weston-desktop-shell-server-protocol.h

BUILT_SOURCES += $(nodist_desktop_shell_la_SOURCES)
endif

if ENABLE_FULLSCREEN_SHELL

module_LTLIBRARIES += fullscreen-shell.la

fullscreen_shell_la_CPPFLAGS =			\
	-I$(top_builddir)/protocol		\
	-I$(top_srcdir)/shared			\
	-I$(top_builddir)/libweston		\
	-I$(top_srcdir)/libweston		\
	-DIN_WESTON

fullscreen_shell_la_LDFLAGS = -module -avoid-version
fullscreen_shell_la_LIBADD =			\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(COMPOSITOR_LIBS)
fullscreen_shell_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
fullscreen_shell_la_SOURCES =			\
	fullscreen-shell/fullscreen-shell.c	\
	shared/helpers.h
nodist_fullscreen_shell_la_SOURCES =			\
	protocol/fullscreen-shell-unstable-v1-protocol.c		\
	protocol/fullscreen-shell-unstable-v1-server-protocol.h

BUILT_SOURCES += $(nodist_fullscreen_shell_la_SOURCES)
endif

if ENABLE_IVI_SHELL

module_LTLIBRARIES +=				\
	$(ivi_shell)				\
	$(hmi_controller)

ivi_shell = ivi-shell.la
ivi_shell_la_LDFLAGS = -module -avoid-version
ivi_shell_la_LIBADD =				\
	libshared.la				\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(COMPOSITOR_LIBS)
ivi_shell_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
ivi_shell_la_SOURCES =				\
	ivi-shell/ivi-layout-export.h		\
	ivi-shell/ivi-layout-private.h		\
	ivi-shell/ivi-layout-shell.h		\
	ivi-shell/ivi-layout.c			\
	ivi-shell/ivi-layout-transition.c	\
	ivi-shell/ivi-shell.h			\
	ivi-shell/ivi-shell.c			\
	ivi-shell/input-panel-ivi.c		\
	shared/helpers.h
nodist_ivi_shell_la_SOURCES =			\
	protocol/ivi-application-protocol.c		\
	protocol/ivi-application-server-protocol.h

BUILT_SOURCES += $(nodist_ivi_shell_la_SOURCES)

hmi_controller = hmi-controller.la
hmi_controller_la_LDFLAGS = -module -avoid-version
hmi_controller_la_LIBADD =			\
	libshared.la				\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(COMPOSITOR_LIBS)
hmi_controller_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
hmi_controller_la_SOURCES =				\
	ivi-shell/ivi-layout-export.h			\
	ivi-shell/hmi-controller.c			\
	shared/helpers.h
nodist_hmi_controller_la_SOURCES =			\
	protocol/ivi-hmi-controller-protocol.c		\
	protocol/ivi-hmi-controller-server-protocol.h

BUILT_SOURCES += $(nodist_hmi_controller_la_SOURCES)

endif


if ENABLE_SCREEN_SHARING

module_LTLIBRARIES += screen-share.la

screen_share_la_CPPFLAGS = $(AM_CPPFLAGS) -DBINDIR='"$(bindir)"'
screen_share_la_LDFLAGS = -module -avoid-version
screen_share_la_LIBADD =			\
	libshared-cairo.la			\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(COMPOSITOR_LIBS)			\
	$(SCREEN_SHARE_LIBS)
screen_share_la_CFLAGS =			\
	$(COMPOSITOR_CFLAGS)			\
	$(SCREEN_SHARE_CFLAGS)			\
	$(AM_CFLAGS)
screen_share_la_SOURCES =			\
	compositor/screen-share.c		\
	shared/helpers.h
nodist_screen_share_la_SOURCES =			\
	protocol/fullscreen-shell-unstable-v1-protocol.c		\
	protocol/fullscreen-shell-unstable-v1-client-protocol.h

endif

if ENABLE_XWAYLAND

libweston_module_LTLIBRARIES += xwayland.la

xwayland_la_CPPFLAGS =				\
	-I$(top_builddir)/protocol		\
	-I$(top_srcdir)/shared			\
	-I$(top_builddir)/libweston		\
	-I$(top_srcdir)/libweston		\
	-I$(top_builddir)/xwayland		\
	-DMODULEDIR='"$(moduledir)"'		\
	-DLIBEXECDIR='"$(libexecdir)"'

xwayland_la_LDFLAGS = -module -avoid-version
xwayland_la_LIBADD =				\
	libshared-cairo.la			\
	libweston-@LIBWESTON_MAJOR@.la		\
	$(XWAYLAND_LIBS)
xwayland_la_CFLAGS =				\
	$(AM_CFLAGS)				\
	$(COMPOSITOR_CFLAGS)			\
	$(PIXMAN_CFLAGS)			\
	$(CAIRO_CFLAGS)
xwayland_la_SOURCES =				\
	xwayland/xwayland.h			\
	xwayland/xwayland-internal-interface.h	\
	xwayland/window-manager.c		\
	xwayland/selection.c			\
	xwayland/dnd.c				\
	xwayland/launcher.c			\
	xwayland/hash.c				\
	xwayland/hash.h				\
	shared/helpers.h

libwestoninclude_HEADERS += xwayland/xwayland-api.h

endif


#
# Shared utilities
#

noinst_LTLIBRARIES += libshared.la libshared-cairo.la \
	libzunitc.la libzunitcmain.la

libshared_la_CPPFLAGS = \
	-DDATADIR='"$(datadir)"' \
	$(AM_CPPFLAGS)

libshared_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)

libshared_la_SOURCES =				\
	shared/config-parser.c			\
	shared/option-parser.c			\
	shared/config-parser.h			\
	shared/file-util.c			\
	shared/file-util.h			\
	shared/helpers.h			\
	shared/os-compatibility.c		\
	shared/os-compatibility.h		\
	shared/xalloc.c			\
	shared/xalloc.h

libshared_cairo_la_CPPFLAGS = $(libshared_la_CPPFLAGS)

libshared_cairo_la_CFLAGS =			\
	$(AM_CFLAGS)				\
	$(COMPOSITOR_CFLAGS)			\
	$(PIXMAN_CFLAGS)			\
	$(CAIRO_CFLAGS)				\
	$(PANGO_CFLAGS)				\
	$(PNG_CFLAGS)				\
	$(WEBP_CFLAGS)

libshared_cairo_la_LIBADD =			\
	$(PIXMAN_LIBS)				\
	$(CAIRO_LIBS)				\
	$(PANGO_LIBS)				\
	$(PNG_LIBS)				\
	$(WEBP_LIBS)				\
	$(JPEG_LIBS)

libshared_cairo_la_SOURCES =			\
	$(libshared_la_SOURCES)			\
	shared/helpers.h			\
	shared/image-loader.c			\
	shared/image-loader.h			\
	shared/cairo-util.c			\
	shared/frame.c				\
	shared/cairo-util.h

libzunitc_la_SOURCES = \
	tools/zunitc/inc/zunitc/zunitc.h	\
	tools/zunitc/inc/zunitc/zunitc_impl.h	\
	tools/zunitc/src/zuc_base_logger.c	\
	tools/zunitc/src/zuc_base_logger.h	\
	tools/zunitc/src/zuc_collector.c	\
	tools/zunitc/src/zuc_collector.h	\
	tools/zunitc/src/zuc_context.h		\
	tools/zunitc/src/zuc_event.h		\
	tools/zunitc/src/zuc_event_listener.h	\
	tools/zunitc/src/zuc_junit_reporter.c	\
	tools/zunitc/src/zuc_junit_reporter.h	\
	tools/zunitc/src/zuc_types.h		\
	tools/zunitc/src/zunitc_impl.c		\
	shared/helpers.h

libzunitc_la_CFLAGS = \
	$(AM_CFLAGS)				\
	-I$(top_srcdir)/tools/zunitc/inc

libzunitc_la_LIBADD = \
	libshared.la \
	$(CLOCK_GETTIME_LIBS)

if ENABLE_JUNIT_XML
libzunitc_la_CFLAGS += \
	$(LIBXML2_CFLAGS)
libzunitc_la_LIBADD += \
	$(LIBXML2_LIBS)
endif

libzunitcmain_la_SOURCES = \
	tools/zunitc/src/main.c

libzunitcmain_la_CFLAGS = \
	$(AM_CFLAGS)				\
	-I$(top_srcdir)/tools/zunitc/inc

libzunitcmain_la_LIBADD =	\
	libzunitc.la		\
	libshared.la

#
# tests subdirectory
#

TESTS = $(internal_tests) $(shared_tests) $(module_tests) $(weston_tests) $(ivi_tests)

internal_tests = 				\
	internal-screenshot.weston

shared_tests =					\
	config-parser.test			\
	timespec.test				\
	string.test					\
	vertex-clip.test			\
	zuctest

module_tests =					\
	plugin-registry-test.la			\
	surface-test.la				\
	surface-global-test.la

weston_tests =					\
	bad_buffer.weston			\
	keyboard.weston				\
	event.weston				\
	pointer.weston				\
	text.weston				\
	presentation.weston			\
	viewporter.weston			\
	roles.weston				\
	subsurface.weston			\
	subsurface-shot.weston			\
	devices.weston				\
	touch.weston

AM_TESTS_ENVIRONMENT = \
	abs_builddir='$(abs_builddir)'; export abs_builddir; \
	abs_top_srcdir='$(abs_top_srcdir)'; export abs_top_srcdir;

TEST_EXTENSIONS = .la .weston
LA_LOG_COMPILER = $(srcdir)/tests/weston-tests-env
WESTON_LOG_COMPILER = $(srcdir)/tests/weston-tests-env

clean-local:
	-rm -rf logs
	-rm -rf $(DOCDIRS)

# To remove when automake 1.11 support is dropped
export abs_builddir

noinst_LTLIBRARIES +=			\
	weston-test.la			\
	weston-test-desktop-shell.la	\
	$(module_tests)			\
	libtest-runner.la		\
	libtest-client.la

noinst_PROGRAMS +=			\
	$(setbacklight)			\
	$(internal_tests)		\
	$(shared_tests)			\
	$(weston_tests)			\
	$(ivi_tests)			\
	matrix-test

test_module_ldflags = -module -avoid-version -rpath $(libdir)
test_module_libadd =			\
	libweston-@LIBWESTON_MAJOR@.la	\
	$(COMPOSITOR_LIBS)

plugin_registry_test_la_SOURCES = tests/plugin-registry-test.c
plugin_registry_test_la_LIBADD = $(test_module_libadd)
plugin_registry_test_la_LDFLAGS = $(test_module_ldflags)
plugin_registry_test_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)

surface_global_test_la_SOURCES = tests/surface-global-test.c
surface_global_test_la_LIBADD = $(test_module_libadd)
surface_global_test_la_LDFLAGS = $(test_module_ldflags)
surface_global_test_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)

surface_test_la_SOURCES = tests/surface-test.c
surface_test_la_LIBADD = $(test_module_libadd)
surface_test_la_LDFLAGS = $(test_module_ldflags)
surface_test_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)

weston_test_la_LIBADD = libshared.la $(test_module_libadd)
weston_test_la_LDFLAGS = $(test_module_ldflags)
weston_test_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
weston_test_la_SOURCES = 			\
	tests/weston-test.c			\
	shared/helpers.h
nodist_weston_test_la_SOURCES =			\
	protocol/weston-test-protocol.c	\
	protocol/weston-test-server-protocol.h

weston_test_desktop_shell_la_LIBADD = libshared.la libweston-desktop-@LIBWESTON_MAJOR@.la $(COMPOSITOR_LIBS)
weston_test_desktop_shell_la_LDFLAGS = $(test_module_ldflags)
weston_test_desktop_shell_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
weston_test_desktop_shell_la_SOURCES = 			\
	tests/weston-test-desktop-shell.c		\
	shared/helpers.h

if ENABLE_EGL
weston_test_la_CFLAGS += $(EGL_TESTS_CFLAGS)
weston_test_la_LDFLAGS += $(EGL_TESTS_LIBS)
endif

libtest_runner_la_SOURCES =			\
	tests/weston-test-runner.c		\
	tests/weston-test-runner.h
libtest_runner_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)

config_parser_test_SOURCES = tests/config-parser-test.c
config_parser_test_LDADD =	\
	libshared.la		\
	$(COMPOSITOR_LIBS)	\
	libzunitc.la		\
	libzunitcmain.la
config_parser_test_CFLAGS =			\
	$(AM_CFLAGS)				\
	-I$(top_srcdir)/tools/zunitc/inc

timespec_test_SOURCES = tests/timespec-test.c
timespec_test_LDADD =	\
	libshared.la		\
	libzunitc.la		\
	libzunitcmain.la
timespec_test_CFLAGS =			\
	$(AM_CFLAGS)				\
	-I$(top_srcdir)/tools/zunitc/inc

string_test_SOURCES = \
	tests/string-test.c \
	shared/string-helpers.h
string_test_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
string_test_LDADD =	libtest-client.la

vertex_clip_test_SOURCES =			\
	tests/vertex-clip-test.c		\
	shared/helpers.h			\
	libweston/vertex-clipping.c		\
	libweston/vertex-clipping.h
vertex_clip_test_LDADD = libtest-runner.la -lm $(CLOCK_GETTIME_LIBS)

libtest_client_la_SOURCES =			\
	tests/weston-test-client-helper.c	\
	tests/weston-test-client-helper.h	\
	tests/input-timestamps-helper.c		\
	tests/input-timestamps-helper.h
nodist_libtest_client_la_SOURCES =				\
	protocol/weston-test-protocol.c				\
	protocol/weston-test-client-protocol.h			\
	protocol/input-timestamps-unstable-v1-protocol.c	\
	protocol/input-timestamps-unstable-v1-client-protocol.h
libtest_client_la_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS) $(CAIRO_CFLAGS)
libtest_client_la_LIBADD = libshared.la libtest-runner.la $(TEST_CLIENT_LIBS) $(CAIRO_LIBS)


#
# Internal tests - tests functionality of the testsuite itself
#

internal_screenshot_weston_SOURCES = tests/internal-screenshot-test.c
internal_screenshot_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
internal_screenshot_weston_LDADD = libtest-client.la


#
# Weston Tests
#

bad_buffer_weston_SOURCES = tests/bad-buffer-test.c
bad_buffer_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
bad_buffer_weston_LDADD = libtest-client.la

keyboard_weston_SOURCES = tests/keyboard-test.c
keyboard_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
keyboard_weston_LDADD = libtest-client.la

event_weston_SOURCES = tests/event-test.c
event_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
event_weston_LDADD = libtest-client.la

pointer_weston_SOURCES = tests/pointer-test.c
pointer_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
pointer_weston_LDADD = libtest-client.la

devices_weston_SOURCES = tests/devices-test.c
devices_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
devices_weston_LDADD = libtest-client.la

text_weston_SOURCES = tests/text-test.c
nodist_text_weston_SOURCES =			\
	protocol/text-input-unstable-v1-protocol.c		\
	protocol/text-input-unstable-v1-client-protocol.h
text_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
text_weston_LDADD = libtest-client.la

subsurface_weston_SOURCES = tests/subsurface-test.c
subsurface_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
subsurface_weston_LDADD = libtest-client.la

subsurface_shot_weston_SOURCES = tests/subsurface-shot-test.c
subsurface_shot_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
subsurface_shot_weston_LDADD = libtest-client.la

presentation_weston_SOURCES = 			\
	tests/presentation-test.c		\
	shared/helpers.h
nodist_presentation_weston_SOURCES =		\
	protocol/presentation-time-protocol.c	\
	protocol/presentation-time-client-protocol.h
presentation_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
presentation_weston_LDADD = libtest-client.la

roles_weston_SOURCES = tests/roles-test.c
roles_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
roles_weston_LDADD = libtest-client.la

viewporter_weston_SOURCES = 			\
	tests/viewporter-test.c		\
	shared/helpers.h
nodist_viewporter_weston_SOURCES =		\
	protocol/viewporter-protocol.c	\
	protocol/viewporter-client-protocol.h
viewporter_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
viewporter_weston_LDADD = libtest-client.la

touch_weston_SOURCES = tests/touch-test.c
touch_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
touch_weston_LDADD = libtest-client.la

if ENABLE_XWAYLAND_TEST
weston_tests +=	xwayland-test.weston
xwayland_test_weston_SOURCES = tests/xwayland-test.c
xwayland_test_weston_CFLAGS = \
	$(AM_CFLAGS) $(XWAYLAND_TEST_CFLAGS) -DXSERVER_PATH='"@XSERVER_PATH@"'
xwayland_test_weston_LDADD = libtest-client.la $(XWAYLAND_TEST_LIBS)
endif

matrix_test_SOURCES =				\
	tests/matrix-test.c			\
	shared/matrix.c				\
	shared/matrix.h
matrix_test_CPPFLAGS = -DUNIT_TEST
matrix_test_LDADD = -lm $(CLOCK_GETTIME_LIBS)

if ENABLE_IVI_SHELL
module_tests += 				\
	ivi-layout-internal-test.la		\
	ivi-layout-test.la

ivi_layout_internal_test_la_LIBADD = $(test_module_libadd)
ivi_layout_internal_test_la_LDFLAGS = $(test_module_ldflags)
ivi_layout_internal_test_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
ivi_layout_internal_test_la_SOURCES =			\
	tests/ivi_layout-internal-test.c

ivi_layout_test_la_LIBADD = $(test_module_libadd)
ivi_layout_test_la_LDFLAGS = $(test_module_ldflags)
ivi_layout_test_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
ivi_layout_test_la_SOURCES =			\
	tests/ivi_layout-test-plugin.c		\
	tests/ivi-test.h			\
	shared/helpers.h
nodist_ivi_layout_test_la_SOURCES =		\
	protocol/weston-test-protocol.c		\
	protocol/weston-test-server-protocol.h

ivi_tests =					\
	ivi-shell-app.weston

ivi_shell_app_weston_SOURCES = tests/ivi-shell-app-test.c
nodist_ivi_shell_app_weston_SOURCES =		\
	protocol/ivi-application-protocol.c	\
	protocol/ivi-application-client-protocol.h
ivi_shell_app_weston_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
ivi_shell_app_weston_LDADD = libtest-client.la

noinst_PROGRAMS += ivi-layout.ivi

ivi_layout_ivi_SOURCES =			\
	tests/ivi_layout-test.c 		\
	tests/ivi-test.h			\
	shared/helpers.h
nodist_ivi_layout_ivi_SOURCES = 		\
	protocol/ivi-application-protocol.c	\
	protocol/ivi-application-client-protocol.h
ivi_layout_ivi_CFLAGS = $(AM_CFLAGS) $(TEST_CLIENT_CFLAGS)
ivi_layout_ivi_LDADD = libtest-client.la
endif

if BUILD_SETBACKLIGHT
noinst_PROGRAMS += setbacklight
setbacklight_SOURCES =				\
	tests/setbacklight.c			\
	libweston/libbacklight.c		\
	libweston/libbacklight.h
setbacklight_CFLAGS = $(AM_CFLAGS) $(SETBACKLIGHT_CFLAGS)
setbacklight_LDADD = $(SETBACKLIGHT_LIBS)
endif

all-local: zuctest$(EXEEXT)

noinst_PROGRAMS += zuctest$(EXEEXT)

zuctest_LDADD =			\
	libzunitc.la		\
	libzunitcmain.la

zuctest_CFLAGS =				\
	$(AM_CFLAGS)				\
	-I$(top_srcdir)/tools/zunitc/inc

zuctest_SOURCES =				\
	tools/zunitc/test/fixtures_test.c	\
	tools/zunitc/test/zunitc_test.c

EXTRA_DIST +=							\
	tests/internal-screenshot.ini				\
	tests/reference/internal-screenshot-bad-00.png		\
	tests/reference/internal-screenshot-good-00.png		\
	tests/reference/subsurface_z_order-00.png		\
	tests/reference/subsurface_z_order-01.png		\
	tests/reference/subsurface_z_order-02.png		\
	tests/reference/subsurface_z_order-03.png		\
	tests/reference/subsurface_z_order-04.png		\
	tests/weston-tests-env

BUILT_SOURCES +=				\
	protocol/weston-test-protocol.c	\
	protocol/weston-test-server-protocol.h	\
	protocol/weston-test-client-protocol.h	\
	protocol/text-input-unstable-v1-protocol.c		\
	protocol/text-input-unstable-v1-client-protocol.h

EXTRA_DIST +=					\
	protocol/weston-desktop-shell.xml	\
	protocol/weston-screenshooter.xml	\
	protocol/text-cursor-position.xml	\
	protocol/weston-test.xml		\
	protocol/ivi-application.xml		\
	protocol/ivi-hmi-controller.xml

#
# manual test modules in tests subdirectory
#

noinst_LTLIBRARIES +=				\
	surface-screenshot.la

surface_screenshot_la_LIBADD = libshared.la $(test_module_libadd)
surface_screenshot_la_LDFLAGS = $(test_module_ldflags)
surface_screenshot_la_CFLAGS = $(AM_CFLAGS) $(COMPOSITOR_CFLAGS)
surface_screenshot_la_SOURCES = tests/surface-screenshot.c


#
# Documentation
#

man_MANS = weston.1 weston.ini.5

if ENABLE_DRM_COMPOSITOR
man_MANS += weston-drm.7
endif

if ENABLE_RDP_COMPOSITOR
man_MANS += weston-rdp.7
endif

MAN_SUBSTS =								\
	-e 's|__weston_native_backend__|$(WESTON_NATIVE_BACKEND)|g'	\
	-e 's|__weston_modules_dir__|$(pkglibdir)|g'			\
	-e 's|__weston_shell_client__|$(WESTON_SHELL_CLIENT)|g'		\
	-e 's|__version__|$(PACKAGE_VERSION)|g'

SUFFIXES = .1 .5 .7 .man

%.1 %.5 %.7 : man/%.man
	$(AM_V_GEN)$(SED) $(MAN_SUBSTS) < $< > $@

EXTRA_DIST +=					\
	man/weston.man				\
	man/weston-drm.man			\
	man/weston-rdp.man			\
	man/weston.ini.man

CLEANFILES += $(man_MANS)

if ENABLE_DEVDOCS
DOXYGEN_INDICES = docs/developer/html/index.html docs/tools/html/index.html

docs/developer/html/index.html: doc/doxygen/tooldev.doxygen | docs/developer
	cd doc/doxygen && $(DOXYGEN) tooldev.doxygen

docs/tools/html/index.html: doc/doxygen/tools.doxygen | docs/tools
	cd doc/doxygen && $(DOXYGEN) tools.doxygen
endif

DOCDIRS =		\
	docs/developer	\
	docs/tools

$(DOCDIRS):
	$(MKDIR_P) $@

.PHONY: doc $(DOXYGEN_INDICES)

doc: $(DOXYGEN_INDICES)

.SECONDEXPANSION:

define protostability
$(if $(findstring unstable,$1),unstable,stable)
endef

define protoname
$(shell echo $1 | $(SED) 's/\([a-z\-]\+\)-[a-z]\+-v[0-9]\+/\1/')
endef

protocol/%-protocol.c : $(WAYLAND_PROTOCOLS_DATADIR)/$$(call protostability,$$*)/$$(call protoname,$$*)/$$*.xml
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(wayland_scanner) code < $< > $@

protocol/%-server-protocol.h : $(WAYLAND_PROTOCOLS_DATADIR)/$$(call protostability,$$*)/$$(call protoname,$$*)/$$*.xml
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(wayland_scanner) server-header < $< > $@

protocol/%-client-protocol.h : $(WAYLAND_PROTOCOLS_DATADIR)/$$(call protostability,$$*)/$$(call protoname,$$*)/$$*.xml
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(wayland_scanner) client-header < $< > $@

protocol/%-protocol.c : $(top_srcdir)/protocol/%.xml
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(wayland_scanner) code < $< > $@

protocol/%-server-protocol.h : $(top_srcdir)/protocol/%.xml
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(wayland_scanner) server-header < $< > $@

protocol/%-client-protocol.h : $(top_srcdir)/protocol/%.xml
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && $(wayland_scanner) client-header < $< > $@
