pkt_hdrs_inc = include_directories('include')

deps = [
  dependency('utils_dep'),
]

pkt_hdrs_lib = static_library(
  'pkt_hdrs',
  include_directories : pkt_hdrs_inc,
  dependencies : deps,
  install : false,
)

pkt_hdrs_dep = declare_dependency(
  link_with : pkt_hdrs_lib,
  include_directories : pkt_hdrs_inc,
  dependencies : deps,
)

meson.override_dependency('pkt_hdrs_dep', pkt_hdrs_dep)
