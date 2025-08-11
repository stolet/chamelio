udp_fast_inc = include_directories('include')

udp_fast_lib = static_library(
  'udp_fast',
  'udp_fast.c',
  include_directories : udp_fast_inc,
  install : false,
)

udp_fast_dep = declare_dependency(
  link_with : udp_fast_lib,
  include_directories : udp_fast_inc,
)

meson.override_dependency('udp_fast_dep', udp_fast_dep)
