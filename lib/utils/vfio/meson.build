vfio_inc = include_directories('include', '../include')

vfio_srcs = files('vfio.c')

deps = [
  dependency('log_dep'),
]

vfio_lib = static_library(
  'vfio',
  vfio_srcs,
  include_directories : vfio_inc,
  dependencies : deps,
  install : false,
)

vfio_dep = declare_dependency(
  link_with : vfio_lib,
  include_directories : vfio_inc,
)

meson.override_dependency('vfio_dep', vfio_dep)