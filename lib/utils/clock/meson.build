clock_inc = include_directories('include', '../include')

clock_srcs = files('clock.c')

deps = [
  dependency('log_dep'),
]

clock_lib = static_library(
  'clock',
  clock_srcs,
  include_directories : clock_inc,
  dependencies : deps,
  install : false,
)

clock_dep = declare_dependency(
  link_with : clock_lib,
  include_directories : clock_inc,
  dependencies : deps,
)

meson.override_dependency('clock_dep', clock_dep)