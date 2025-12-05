arp_inc = include_directories('include')

arp_srcs = files('arp.c')

deps = [
  dependency('queue_dep'),
  dependency('pkt_hdrs_dep'),
  dependency('tomgr_dep'),
]

arp_lib = static_library(
  'arp',
  arp_srcs,
  include_directories : arp_inc,
  dependencies : deps,
  install : false,
)

arp_dep = declare_dependency(
  link_with : arp_lib,
  dependencies : deps,
  include_directories : arp_inc,
)

meson.override_dependency('arp_dep', arp_dep)