subdir('queue')
subdir('fast')
subdir('slow')
subdir('lib')

udp_lib = static_library(
  'udp',
  install : false,
)

deps = [
  dependency('fast_dep'),
  dependency('control_dep'),
]

udp_dep = declare_dependency(
  link_with : udp_lib,
  dependencies : deps,
)

meson.override_dependency('udp_dep', udp_dep)