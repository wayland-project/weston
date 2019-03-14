dep_scanner = dependency('wayland-scanner', native: true)
prog_scanner = find_program(dep_scanner.get_pkgconfig_variable('wayland_scanner'))

# XXX: For minor version 2 of zwp_linux_explicit_synchronization_v1, we
# actually need a development version after 1.17, but there is no way to
# express such a requirement at the moment.
dep_wp = dependency('wayland-protocols', version: '>= 1.17')
dir_wp_base = dep_wp.get_pkgconfig_variable('pkgdatadir')

install_data(
	[
		'weston-debug.xml',
	],
	install_dir: join_paths(dir_data, dir_protocol_libweston)
)

generated_protocols = [
	[ 'input-method', 'v1' ],
	[ 'input-method', 'v2' ],
	[ 'input-timestamps', 'v1' ],
	[ 'ivi-application', 'internal' ],
	[ 'ivi-hmi-controller', 'internal' ],
	[ 'fullscreen-shell', 'v1' ],
	[ 'linux-dmabuf', 'v1' ],
	[ 'linux-explicit-synchronization', 'v1' ],
	[ 'presentation-time', 'stable' ],
	[ 'pointer-constraints', 'v1' ],
	[ 'relative-pointer', 'v1' ],
	[ 'tablet', 'v2' ],
	[ 'text-cursor-position', 'internal' ],
	[ 'text-input', 'v1' ],
	[ 'text-input', 'v3' ],
	[ 'viewporter', 'stable' ],
	[ 'virtual-keyboard', 'v1' ],
	[ 'weston-debug', 'internal' ],
	[ 'weston-desktop-shell', 'internal' ],
	[ 'weston-input-panel', 'internal' ],
	[ 'weston-screenshooter', 'internal' ],
	[ 'weston-test', 'internal' ],
	[ 'weston-touch-calibration', 'internal' ],
	[ 'xdg-output', 'v1' ],
	[ 'xdg-shell', 'v6' ],
	[ 'xdg-shell', 'stable' ],
]

foreach proto: generated_protocols
	proto_name = proto[0]
	if proto[1] == 'internal'
		base_file = proto_name
		xml_path = '@0@.xml'.format(proto_name)
	elif proto[1] == 'stable'
		base_file = proto_name
		xml_path = '@0@/stable/@1@/@1@.xml'.format(dir_wp_base, base_file)
	else
		base_file = '@0@-unstable-@1@'.format(proto_name, proto[1])
		xml_path = '@0@/unstable/@1@/@2@.xml'.format(dir_wp_base, proto_name, base_file)
	endif

	foreach output_type: [ 'client-header', 'server-header', 'private-code' ]
		if output_type == 'client-header'
			output_file = '@0@-client-protocol.h'.format(base_file)
		elif output_type == 'server-header'
			output_file = '@0@-server-protocol.h'.format(base_file)
		else
			output_file = '@0@-protocol.c'.format(base_file)
			if dep_scanner.version().version_compare('< 1.14.91')
				output_type = 'code'
			endif
		endif

		var_name = output_file.underscorify()
		target = custom_target(
			'@0@ @1@'.format(base_file, output_type),
			command: [ prog_scanner, output_type, '@INPUT@', '@OUTPUT@' ],
			input: xml_path,
			output: output_file,
		)

		set_variable(var_name, target)
	endforeach
endforeach
