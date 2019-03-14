srcs_weston = [
	git_version_h,
	'main.c',
	'text-backend.c',
	'weston-screenshooter.c',
	input_method_unstable_v2_server_protocol_h,
	input_method_unstable_v2_protocol_c,
	text_input_unstable_v3_server_protocol_h,
	text_input_unstable_v3_protocol_c,
	virtual_keyboard_unstable_v1_server_protocol_h,
	virtual_keyboard_unstable_v1_protocol_c,
	weston_input_panel_server_protocol_h,
	weston_input_panel_protocol_c,
	weston_screenshooter_server_protocol_h,
	weston_screenshooter_protocol_c,
]
deps_weston = [
	dep_libshared,
	dep_libweston,
	dep_libinput,
	dep_libevdev,
	dep_libdl,
	dep_threads,
]

if get_option('xwayland')
	config_h.set('BUILD_XWAYLAND', '1')

	srcs_weston += 'xwayland.c'
	config_h.set_quoted('XSERVER_PATH', get_option('xwayland-path'))
endif

exe_weston = executable(
	'weston',
	srcs_weston,
	include_directories: include_directories('..', '../shared'),
	link_args: [ '-Wl,-export-dynamic' ],
	dependencies: deps_weston,
	install: true
)
install_headers('weston.h', subdir: 'weston')

pkgconfig.generate(
	filebase: 'weston',
	name: 'Weston Plugin API',
	version: version_weston,
	description: 'Header files for Weston plugin development',
	requires_private: [ lib_weston ],
	variables: [
		'libexecdir=' + join_paths('${prefix}', get_option('libexecdir')),
		'pkglibexecdir=${libexecdir}/weston'
	],
	subdirs: 'weston'
)

install_data(
	'weston.desktop',
	install_dir: join_paths(dir_data, 'wayland-sessions')
)

if get_option('screenshare')
	srcs_screenshare = [
		'screen-share.c',
		fullscreen_shell_unstable_v1_client_protocol_h,
		fullscreen_shell_unstable_v1_protocol_c,
	]
	deps_screenshare = [
		dep_libshared,
		dep_libweston,
		dep_wayland_client,
	]
	plugin_screenshare = shared_library(
		'screen-share',
		srcs_screenshare,
		include_directories: include_directories('..', '../shared'),
		dependencies: deps_screenshare,
		name_prefix: '',
		install: true,
		install_dir: dir_module_weston
	)
	env_modmap += 'screen-share.so=@0@;'.format(plugin_screenshare.full_path())
endif

if get_option('color-management-lcms')
	config_h.set('HAVE_LCMS', '1')

	srcs_lcms = [
		'cms-static.c',
		'cms-helper.c',
	]

	dep_lcms2 = dependency('lcms2', required: false)
	if not dep_lcms2.found()
		error('cms-static requires lcms2 which was not found. Or, you can use \'-Dcolor-management-lcms=false\'.')
	endif

	plugin_lcms = shared_library(
		'cms-static',
		srcs_lcms,
		include_directories: include_directories('..', '../shared'),
		dependencies: [ dep_libweston, dep_lcms2 ],
		name_prefix: '',
		install: true,
		install_dir: dir_module_weston
	)
	env_modmap += 'cms-static.so=@0@;'.format(plugin_lcms.full_path())
endif

if get_option('color-management-colord')
	if not get_option('color-management-lcms')
		error('LCMS must be enabled to support colord')
	endif

	srcs_colord = [
		'cms-colord.c',
		'cms-helper.c',
	]

	dep_colord = dependency('colord', version: '>= 0.1.27', required: false)
	if not dep_colord.found()
		error('cms-colord requires colord >= 0.1.27 which was not found. Or, you can use \'-Dcolor-management-colord=false\'.')
	endif

	plugin_colord_deps = [ dep_libweston, dep_colord ]

	foreach depname : [ 'glib-2.0', 'gobject-2.0' ]
		dep = dependency(depname, required: false)
		if not dep.found()
			error('cms-colord requires \'@0@\' which was not found. If you rather not build this, set \'-Dcolor-management-colord=false\'.'.format(depname))
		endif
		plugin_colord_deps += dep
	endforeach

	plugin_colord = shared_library(
		'cms-colord',
		srcs_colord,
		include_directories: include_directories('..', '../shared'),
		dependencies: plugin_colord_deps,
		name_prefix: '',
		install: true,
		install_dir: dir_module_weston
	)
	env_modmap += 'cms-colord.so=@0@;'.format(plugin_colord.full_path())
endif

if get_option('systemd')
	dep_libsystemd = dependency('libsystemd', required: false)
	if not dep_libsystemd.found()
		error('systemd-notify requires libsystemd which was not found. Or, you can use \'-Dsystemd=false\'.')
	endif

	plugin_systemd_notify = shared_library(
		'systemd-notify',
		'systemd-notify.c',
		include_directories: include_directories('..', '../shared'),
		dependencies: [ dep_libweston, dep_libsystemd ],
		name_prefix: '',
		install: true,
		install_dir: dir_module_weston
	)
	env_modmap += 'systemd-notify.so=@0@;'.format(plugin_systemd_notify.full_path())
endif

weston_ini_config = configuration_data()
weston_ini_config.set('bindir', dir_bin)
weston_ini_config.set('libexecdir', dir_libexec)
weston_ini_config.set('abs_top_srcdir', meson.source_root())
weston_ini_config.set('abs_top_builddir', meson.build_root())
configure_file(
	input: '../weston.ini.in',
	output: 'weston.ini',
	configuration: weston_ini_config
)
