deps_libweston = [
	dep_wayland_server,
	dep_pixman,
	dep_libm,
	dep_libdl,
	dep_libdrm_headers,
	dep_xkbcommon,
	dep_matrix_c
]
srcs_libweston = [
	git_version_h,
	'animation.c',
	'bindings.c',
	'clipboard.c',
	'compositor.c',
	'content-protection.c',
	'data-device.c',
	'input.c',
	'linux-dmabuf.c',
	'linux-explicit-synchronization.c',
	'linux-sync-file.c',
	'log.c',
	'noop-renderer.c',
	'pixel-formats.c',
	'pixman-renderer.c',
	'plugin-registry.c',
	'screenshooter.c',
	'timeline.c',
	'touch-calibration.c',
	'weston-log-wayland.c',
	'weston-log-file.c',
	'weston-log-flight-rec.c',
	'weston-log.c',
	'weston-direct-display.c',
	'zoom.c',
	linux_dmabuf_unstable_v1_protocol_c,
	linux_dmabuf_unstable_v1_server_protocol_h,
	linux_explicit_synchronization_unstable_v1_protocol_c,
	linux_explicit_synchronization_unstable_v1_server_protocol_h,
	input_method_unstable_v1_protocol_c,
	input_method_unstable_v1_server_protocol_h,
	input_timestamps_unstable_v1_protocol_c,
	input_timestamps_unstable_v1_server_protocol_h,
	presentation_time_protocol_c,
	presentation_time_server_protocol_h,
	pointer_constraints_unstable_v1_protocol_c,
	pointer_constraints_unstable_v1_server_protocol_h,
	relative_pointer_unstable_v1_protocol_c,
	relative_pointer_unstable_v1_server_protocol_h,
	weston_screenshooter_protocol_c,
	weston_screenshooter_server_protocol_h,
	text_cursor_position_protocol_c,
	text_cursor_position_server_protocol_h,
	text_input_unstable_v1_protocol_c,
	text_input_unstable_v1_server_protocol_h,
	weston_touch_calibration_protocol_c,
	weston_touch_calibration_server_protocol_h,
	weston_content_protection_protocol_c,
	weston_content_protection_server_protocol_h,
	viewporter_protocol_c,
	viewporter_server_protocol_h,
	xdg_output_unstable_v1_protocol_c,
	xdg_output_unstable_v1_server_protocol_h,
	weston_debug_protocol_c,
	weston_debug_server_protocol_h,
	weston_direct_display_protocol_c,
	weston_direct_display_server_protocol_h,
]

if get_option('renderer-gl')
	dep_egl = dependency('egl', required: false)
	if not dep_egl.found()
		error('libweston + gl-renderer requires egl which was not found. Or, you can use \'-Drenderer-gl=false\'.')
	endif
	deps_libweston += dep_egl
endif

lib_weston = shared_library(
	'weston-@0@'.format(libweston_major),
	srcs_libweston,
	include_directories: common_inc,
	install: true,
	version: '0.0.@0@'.format(libweston_revision),
	link_whole: lib_libshared,
	dependencies: deps_libweston
)

deps_for_libweston_users = [
	dep_wayland_server,
	dep_pixman,
	dep_xkbcommon,
]

# For external users, like Weston.
dep_libweston_public = declare_dependency(
	link_with: lib_weston,
	include_directories: public_inc,
	dependencies: deps_for_libweston_users
)

# For internal users, like the backends.
dep_libweston_private = declare_dependency(
	link_with: lib_weston,
	include_directories: [ include_directories('.'), public_inc ],
	dependencies: deps_for_libweston_users
)

# XXX: We should be able to use dep_libweston_private.partial_dependency() instead
# of this, but a Meson bug makes it not work. It will be fixed with
# https://github.com/mesonbuild/meson/pull/5167
# in hopefully Meson 0.51.
dep_libweston_private_h_deps = []
foreach d : deps_for_libweston_users
	dep_libweston_private_h_deps += d.partial_dependency(compile_args: true)
endforeach
dep_libweston_private_h = declare_dependency(
	include_directories: [ include_directories('.'), public_inc ],
	dependencies: dep_libweston_private_h_deps
)

pkgconfig.generate(
	lib_weston,
	filebase: 'libweston-@0@'.format(libweston_major),
	name: 'libweston API',
	version: version_weston,
	description: 'Header files for libweston compositors development',
	requires_private: deps_for_libweston_users,
	subdirs: dir_include_libweston
)

pkgconfig.generate(
	filebase: 'libweston-@0@-protocols'.format(libweston_major),
	name: 'libWeston Protocols',
	version: version_weston,
	description: 'libWeston protocol files',
	variables: [
		'datarootdir=' + join_paths('${prefix}', get_option('datadir')),
		'pkgdatadir=' + join_paths('${pc_sysrootdir}${datarootdir}', dir_protocol_libweston)
	],
	install_dir: dir_data_pc
)

srcs_session_helper = [
	'launcher-direct.c',
	'launcher-util.c',
	'launcher-weston-launch.c',
]
deps_session_helper = [ dep_libweston_private_h ]

if get_option('backend-drm')
	deps_session_helper += dep_libdrm
endif

systemd_dep = dependency('', required: false)
if get_option('launcher-logind')
	systemd_dep = dependency('libsystemd', version: '>= 209', required: false)
	if systemd_dep.found()
		config_h.set('HAVE_SYSTEMD_LOGIN_209', '1')
	else
		systemd_dep = dependency('libsystemd-login', version: '>= 198', required: false)
		if not systemd_dep.found()
			error('logind support requires libsystemd or libsystemd-login but neither was found. Or, you can use \'-Dlauncher-logind=false\'')
		endif
	endif

	dbus_dep = dependency('dbus-1', version: '>= 1.6', required: false)
	if not dbus_dep.found()
		error('logind support requires dbus-1 >= 1.6 which was not found. Or, you can use \'-Dlauncher-logind=false\'')
	endif

	config_h.set('HAVE_DBUS', '1')
	config_h.set('HAVE_SYSTEMD_LOGIN', '1')

	srcs_session_helper += [
		'dbus.c',
		'launcher-logind.c',
	]
	deps_session_helper += [
		dbus_dep,
		systemd_dep,
	]
endif

lib_session_helper = static_library(
	'session-helper',
	srcs_session_helper,
	include_directories: common_inc,
	dependencies: deps_session_helper,
	install: false
)
dep_session_helper = declare_dependency(link_with: lib_session_helper)


lib_libinput_backend = static_library(
	'libinput-backend',
	[
		'libinput-device.c',
		'libinput-seat.c'
	],
	dependencies: [
		dep_libweston_private,
		dep_libinput,
		dependency('libudev', version: '>= 136')
	],
	include_directories: common_inc,
	install: false
)
dep_libinput_backend = declare_dependency(
	link_with: lib_libinput_backend,
	include_directories: include_directories('.')
)

dep_vertex_clipping = declare_dependency(
	sources: 'vertex-clipping.c',
	include_directories: include_directories('.')
)

if get_option('weston-launch')
	dep_pam = cc.find_library('pam')

	if not cc.has_function('pam_open_session', dependencies: dep_pam)
		error('pam_open_session not found for weston-launch')
	endif

	executable(
		'weston-launch',
		'weston-launch.c',
		dependencies: [dep_pam, systemd_dep, dep_libdrm],
		include_directories: common_inc,
		install: true
	)

	meson.add_install_script('echo', 'REMINDER: You are installing weston-launch, please make it setuid-root.')
endif

subdir('renderer-gl')
subdir('backend-drm')
subdir('backend-fbdev')
subdir('backend-headless')
subdir('backend-rdp')
subdir('backend-vnc')
subdir('backend-wayland')
subdir('backend-x11')
