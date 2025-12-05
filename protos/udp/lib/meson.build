udp_lib_inc = include_directories('include', '../include')

deps = [
  dependency('queue_dep'),
  dependency('cham'),
]

udp_lib_lib = library(
  'cham_udp',
  'udp_lib.c',
  include_directories : udp_lib_inc,
  version : '1.0.0',
  dependencies : deps,
  install : true,
)

udp_lib_dep = declare_dependency(
  link_with : udp_lib_lib,
  include_directories : udp_lib_inc,
  dependencies: deps,
)

meson.override_dependency('cham_udp_dep', udp_lib_dep)

# Install headers
install_headers('include/udp_lib.h', subdir: 'chamelio')

# Generate pkgconfig file
pkg = import('pkgconfig')
pkg.generate(
  udp_lib_lib,
  description : 'Chamelio UDP protocol library',
  subdirs : 'chamelio'
)
