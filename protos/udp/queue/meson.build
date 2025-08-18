udp_queue_inc = include_directories('include')

udp_queue_srcs = files('udp_queue.c')

deps = [
  dependency('log_dep'),
  dependency('shm_dep'),
  dependency('queue_dep'),
]

udp_queue_lib = static_library(
  'udp_queue',
  udp_queue_srcs,
  include_directories : udp_queue_inc,
  dependencies : deps,
  install : false,
)

udp_queue_dep = declare_dependency(
  link_with : udp_queue_lib,
  include_directories : udp_queue_inc,
  dependencies : deps,
)

meson.override_dependency('udp_queue_dep', udp_queue_dep)