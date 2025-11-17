subdir('log')
subdir('uxsocket')
subdir('shm')
subdir('queue')
subdir('scheduler')
subdir('pkt_hdrs')

utils_inc = include_directories('include')

utils_lib = static_library(
  'utils',
  include_directories : utils_inc,
  install : false,
)

deps = [
  dependency('log_dep'),
  dependency('uxsocket_dep'),
  dependency('shm_dep'),
  dependency('queue_dep'),
  dependency('scheduler_dep'),
  dependency('pkt_hdrs_dep'),
]

utils_dep = declare_dependency(
  link_with : utils_lib,
  include_directories : utils_inc,
  dependencies : deps,
)

meson.override_dependency('utils_dep', utils_dep)