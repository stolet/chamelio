tcp_inc = include_directories('include', '../include')

deps = [
  dependency('queue_dep'),
  dependency('cham'),
]

tcp_lib = library(
  'cham_tcp',
  'tcp_lib.c',
  include_directories : tcp_inc,
  version : '1.0.0',
  dependencies : deps,
  install : true,
)

tcp_dep = declare_dependency(
  link_with : tcp_lib,
  include_directories : tcp_inc,
  dependencies: deps,
)

meson.override_dependency('cham_tcp_dep', tcp_dep)

# Install headers
install_headers('include/tcp_lib.h', subdir: 'chamelio')

# Generate pkgconfig file
pkg = import('pkgconfig')
pkg.generate(
  tcp_lib,
  description : 'Chamelio TCP protocol library',
  subdirs : 'chamelio'
)
