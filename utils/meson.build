subdir('log')
subdir('uxsocket')

utils_inc = include_directories('include')

utils_lib = static_library(
  'utils',
  include_directories : utils_inc,
  install : false
)

deps = [
  dependency('log_dep'),
  dependency('uxsocket_dep'),
]

utils_dep = declare_dependency(
  link_with : utils_lib,
  include_directories : utils_inc,
  dependencies : deps,
)

meson.override_dependency('utils_dep', utils_dep)