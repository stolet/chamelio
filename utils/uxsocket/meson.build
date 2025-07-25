uxsocket_inc = include_directories('include')

deps = [
  dependency('log_dep'),
]

uxsocket_lib = static_library(
  'uxsocket',
  'uxsocket.c',
  include_directories : uxsocket_inc,
  dependencies: deps,
  install : false,
)

uxsocket_dep = declare_dependency(
  link_with : uxsocket_lib,
  include_directories : uxsocket_inc,
)

meson.override_dependency('uxsocket_dep', uxsocket_dep)
