rpc_lib_inc = include_directories('include', '../include')

deps = [
  dependency('queue_dep'),
  dependency('cham'),
]

rpc_lib_lib = library(
  'cham_rpc',
  'rpc_lib.c',
  include_directories : rpc_lib_inc,
  version : '1.0.0',
  dependencies : deps,
  install : true,
)

rpc_lib_dep = declare_dependency(
  link_with : rpc_lib_lib,
  include_directories : rpc_lib_inc,
  dependencies: deps,
)

meson.override_dependency('cham_rpc_dep', rpc_lib_dep)

# Install headers
install_headers('include/rpc_lib.h', subdir: 'chamelio')

# Generate pkgconfig file
pkg = import('pkgconfig')
pkg.generate(
  rpc_lib_lib,
  description : 'Chamelio UDP protocol library',
  subdirs : 'chamelio'
)
