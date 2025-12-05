scheduler_inc = include_directories('include', '../include')

scheduler_srcs = files('scheduler.c')

deps = [
  dependency('log_dep'),
]

scheduler_lib = static_library(
  'scheduler',
  scheduler_srcs,
  include_directories : scheduler_inc,
  dependencies : deps,
  install : false,
)

scheduler_dep = declare_dependency(
  link_with : scheduler_lib,
  include_directories : scheduler_inc,
  dependencies : deps,
)

meson.override_dependency('scheduler_dep', scheduler_dep)