pkt_hdrs_inc = include_directories('include')

pkt_hdrs_lib = static_library(
  'pkt_hdrs',
  include_directories : pkt_hdrs_inc,
  install : false,
)

pkt_hdrs_dep = declare_dependency(
  link_with : pkt_hdrs_lib,
  include_directories : pkt_hdrs_inc,
)

meson.override_dependency('pkt_hdrs_dep', pkt_hdrs_dep)
