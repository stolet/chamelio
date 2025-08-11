udp_inc = include_directories('include')

deps = [
  dependency('utils_dep'),
  dependency('log_dep'),
  dependency('protocols_dep'),
]

udp_lib = static_library(
  'udp',
  include_directories : udp_inc,
  dependencies: deps,
  install : false,
)

udp_old_dep = declare_dependency(
  link_with : udp_lib,
  include_directories : udp_inc,
  dependencies : deps,
)

meson.override_dependency('udp_old_dep', udp_old_dep)