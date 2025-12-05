subdir('fast')
subdir('slow')
subdir('lib')

udp_inc = include_directories('include')

deps = [
  dependency('queue_dep'),
]

udp_dep = declare_dependency(
  dependencies : deps,
  include_directories: udp_inc,
)

meson.override_dependency('udp_dep', udp_dep)