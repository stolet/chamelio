homa_lib = library(
  'cham_homa',
  'homa_lib.c',
  include_directories: [homa_inc, include_directories('include')],
  dependencies: homa_deps,
  version: '1.0.0',
  install: true,
)

meson.override_dependency('cham_homa_dep', declare_dependency(
  link_with: homa_lib,
  include_directories: include_directories('include'),
  dependencies: homa_deps,
))

install_headers('include/homa_lib.h', subdir: 'chamelio')
pkg = import('pkgconfig')
pkg.generate(homa_lib, description: 'Chamelio Homa RPC library',
  subdirs: 'chamelio')
