subdir('queue')
subdir('fast')
subdir('slow')
subdir('lib')

udp_inc = include_directories('include')

deps = [
  dependency('queue_dep'),
]

udp_lib = static_library(
  'udp',
  install : false,
  dependencies: deps,
  include_directories: udp_inc,
)

udp_dep = declare_dependency(
  link_with : udp_lib,
  dependencies : deps,
  include_directories: udp_inc,
)

meson.override_dependency('udp_dep', udp_dep)