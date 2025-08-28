udp_fast_inc = include_directories('include', '../include')

deps = [
  dependency('log_dep'),
  dependency('utils_dep'),
  dependency('chamelio_lib'),
]

udp_fast_lib = static_library(
  'udp_fast',
  'udp_fast.c',
  include_directories : udp_fast_inc,
  dependencies: deps,
  install : false,
)

udp_fast_dep = declare_dependency(
  link_with : udp_fast_lib,
  include_directories : udp_fast_inc,
  dependencies: deps,
)

meson.override_dependency('udp_fast_dep', udp_fast_dep)
