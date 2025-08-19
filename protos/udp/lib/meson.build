udp_lib_inc = include_directories('include')

deps = [
  dependency('queue_dep'),
  dependency('udp_queue_dep'),
  dependency('chamelio_lib'),
]

udp_lib_lib = static_library(
  'udp_lib',
  'udp_lib.c',
  include_directories : udp_lib_inc,
  dependencies : deps,
  install : false,
)

udp_lib_dep = declare_dependency(
  link_with : udp_lib_lib,
  include_directories : udp_lib_inc,
  dependencies: deps,
)

meson.override_dependency('udp_lib_dep', udp_lib_dep)
