# Build utils library first
subdir('utils')

lib_inc = include_directories('include')

lib_srcs = files(
  'guest.c', 
  'ivshmem.c', 
)

deps = [
  dependency('cham_utils_dep'),
  dependency('log_dep'),
  dependency('queue_dep'),
]

lib = library(
  'cham',             
  lib_srcs,
  include_directories : lib_inc,
  version : '1.0.0',
  dependencies : deps,
  install : true
)

lib_dep = declare_dependency(
  link_with : lib,
  include_directories : lib_inc,
  dependencies : deps,
)

meson.override_dependency('cham', lib_dep)

# Install headers
install_headers(
  'include/cham_lib.h',
  'include/cham_fast.h',
  subdir: 'chamelio'
)

pkg = import('pkgconfig')
pkg.generate(
  lib,
  description : 'Chamelio library',
  subdirs : 'chamelio'
)
