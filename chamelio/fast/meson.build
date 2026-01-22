fast_inc = include_directories('include', '../include')

fast_srcs = files(
  'fast.c', 
  'fast_jit.c',
  'controlif.c',
  'infra.c',
)

deps = [
  dependency('cham_config_dep'),
  dependency('cham_utils_dep'),
  dependency('netvirt_dep'),
  dependency('log_pkt_dep'),
  dependency('nic_dep'),
  dependency('queue_dep'),
  dependency('chamelio_dep'),
  dependency('pkt_hdrs_dep'),
  dependency('arp_dep'),
  dependency('udp_dep'),
  dependency('cham'),
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
  dependencies: deps,
)

meson.override_dependency('fast_dep', fast_dep)
