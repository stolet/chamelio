slow_inc = include_directories('include')

slow_srcs = files('slow.c', 'guestif.c', 'appif.c')

deps = [
  dependency('config_dep'),
  dependency('log_dep'),
  dependency('shm_dep'),
  dependency('queue_dep'),
  dependency('bufs_dep'),
  dependency('bufs_dep'),
]

slow_lib = static_library(
  'slow',
  slow_srcs,
  include_directories : slow_inc,
  dependencies : deps,
  install : false,
)

slow_dep = declare_dependency(
  link_with: slow_lib,
  include_directories: slow_inc,
)

meson.override_dependency('slow_dep', slow_dep)