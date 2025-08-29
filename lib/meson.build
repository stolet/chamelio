lib_inc = include_directories('include')

lib_srcs = files(
  'guest.c', 
  'ivshmem.c', 
  'scheduler.c',
)

deps = [
  dependency('utils_dep'),
  dependency('log_dep'),
  dependency('queue_dep'),
]

lib = library(
  'chamelio',             
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

meson.override_dependency('chamelio_lib', lib_dep)

pkg = import('pkgconfig')
pkg.generate(
  lib,
  description : 'Chamelio low level library',
  subdirs : 'chamelio'
)
