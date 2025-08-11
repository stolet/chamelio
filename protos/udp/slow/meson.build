udp_slow_inc = include_directories('include')

udp_slow_lib = static_library(
  'udp_slow',
  'udp_slow.c',
  include_directories : udp_slow_inc,
  install : false,
)

udp_slow_dep = declare_dependency(
  link_with : udp_slow_lib,
  include_directories : udp_slow_inc,
)

meson.override_dependency('udp_slow_dep', udp_slow_dep)
