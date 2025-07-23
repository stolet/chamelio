ip_inc = include_directories('include')

deps = [
  dependency('utils_dep'),
  dependency('log_dep'),
  dependency('protocols_dep'),
]

ip_lib = static_library(
  'ip',
  include_directories : ip_inc,
  dependencies: deps,
  install : false,
)

ip_dep = declare_dependency(
  link_with : ip_lib,
  include_directories : ip_inc,
  dependencies: deps,
)

meson.override_dependency('ip_dep', ip_dep)