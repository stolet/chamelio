control_inc = include_directories('include')

control_srcs = files('control.c', 'ivshmemif.c', 'guestif.c')

deps = [
  dependency('config_dep'),
  dependency('log_dep'),
  dependency('shm_dep'),
  dependency('queue_dep'),
]

control_lib = static_library(
  'control',
  control_srcs,
  include_directories : control_inc,
  dependencies : deps,
  install : false,
)

control_dep = declare_dependency(
  link_with: control_lib,
  include_directories: control_inc,
)

meson.override_dependency('control_dep', control_dep)