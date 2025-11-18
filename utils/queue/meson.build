queue_inc = include_directories('include', '../include')

queue_srcs = files('queue.c')

deps = [
  dependency('log_dep'),
  dependency('shm_dep'),
  dependency('pkt_hdrs_dep'),
]

queue_lib = static_library(
  'queue',
  queue_srcs,
  include_directories : queue_inc,
  dependencies : deps,
  install : false,
)

queue_dep = declare_dependency(
  link_with : queue_lib,
  include_directories : queue_inc,
  dependencies : deps,
)

meson.override_dependency('queue_dep', queue_dep)