log_pkt_inc = include_directories('include')

log_pkt_srcs = files('log_pkt.c')

deps = [
  dependency('utils_dep'),
  dependency('pkt_hdrs_dep'),
]

log_pkt_lib = static_library(
  'log_pkt',
  log_pkt_srcs,
  include_directories : log_pkt_inc,
  dependencies : deps,
  install : false,
)

log_pkt_dep = declare_dependency(
  link_with : log_pkt_lib,
  include_directories : log_pkt_inc,
  dependencies : deps,
)

meson.override_dependency('log_pkt_dep', log_pkt_dep)
