fast_inc = include_directories('include')

fast_srcs = files(
  'fast.c', 
  'fast_process.c',
  'controlif.c'
)

deps = [
  dependency('config_dep'),
  dependency('udp_old_dep'),
  dependency('ip_dep'),
  dependency('eth_dep'),
  dependency('utils_dep'),
  dependency('nic_dep'),
  dependency('queue_dep'),
  dependency('chamelio_dep'),
  dependency('udp_fast_dep'),
]

fast_lib = static_library(
  'fast',
  fast_srcs,
  include_directories : fast_inc,
  dependencies : deps,
  install : false,
)

fast_dep = declare_dependency(
  link_with : fast_lib,
  include_directories : fast_inc,
)

meson.override_dependency('fast_dep', fast_dep)
