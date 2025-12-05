log_inc = include_directories('include', '../include')

log_lib = static_library(
  'log',
  'log.c',
  include_directories : log_inc,
  install : false,
)

log_dep = declare_dependency(
  link_with : log_lib,
  include_directories : log_inc,
)

meson.override_dependency('log_dep', log_dep)
