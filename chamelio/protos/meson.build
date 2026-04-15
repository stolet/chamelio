proto_inc = include_directories('include')

proto_srcs = files(
  'tcp_fast.c',
  'udp_fast.c',
)

deps = [
  dependency('cham_utils_dep'),
  dependency('queue_dep'),
  dependency('pkt_hdrs_dep'),
  dependency('tcp_dep'),
  dependency('udp_dep'),
  dependency('cham'),
  dpdk_dep,
]

fast_proto_lib = static_library(
  'fast_proto',
  proto_srcs,
  include_directories : [proto_inc, '../include'],
  dependencies : deps,
  install : false,
)

fast_proto_dep = declare_dependency(
  link_with : fast_proto_lib,
  include_directories : [proto_inc, '../include'],
  dependencies : deps,
)

meson.override_dependency('fast_proto_dep', fast_proto_dep)
