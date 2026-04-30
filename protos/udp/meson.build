subdir('config')
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

statetool_inc = include_directories('include', 'slow/include')

statetool_deps = [
  dependency('cham'),
  dependency('udp_config_dep'),
]

executable(
  'statetool',
  files('statetool.c'),
  include_directories: statetool_inc,
  dependencies: statetool_deps,
)
