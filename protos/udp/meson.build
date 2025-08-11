subdir('fast')
subdir('slow')

udp_lib = static_library(
  'udp',
  install : false,
)

deps = [
  dependency('fast_dep'),
  dependency('slow_dep'),
]

udp_dep = declare_dependency(
  link_with : udp_lib,
  dependencies : deps,
)

meson.override_dependency('udp_dep', udp_dep)