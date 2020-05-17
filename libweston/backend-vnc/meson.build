if not get_option('backend-vnc')
	subdir_done()
endif

config_h.set('BUILD_VNC_COMPOSITOR', '1')

neatvnc_project = subproject('neatvnc', required: false)
if neatvnc_project.found()
	dep_neatvnc = neatvnc_project.get_variable('neatvnc_dep')
else
	dep_neatvnc = dependency('neatvnc', version: '>= 0.0.0', required: true)
	if not dep_neatvnc.found()
		error('VNC backend requires neatvnc which was not found. Or, you can use \'-Dbackend-vnc=false\'.')
	endif
endif

aml_project = subproject('aml', required: false)
if aml_project.found()
	dep_aml = aml_project.get_variable('aml_dep')
else
	dep_aml = dependency('aml', version: '>= 0.0.0', required: true)
	if not dep_aml.found()
		error('VNC backend requires libaml which was not found. Or, you can use \'-Dbackend-vnc=false\'.')
	endif
endif

deps_vnc = [
	dep_libweston_private,
	dep_neatvnc,
	dep_aml,
	dep_libdrm_headers,
]
plugin_vnc = shared_library(
	'vnc-backend',
	[ 'vnc.c' ],
	include_directories: include_directories('../..', '../../shared'),
	dependencies: deps_vnc,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston
)
env_modmap += 'vnc-backend.so=@0@;'.format(plugin_vnc.full_path())
install_headers(backend_vnc_h, subdir: dir_include_libweston_install)
