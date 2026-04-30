subdir('config')
subdir('fast')
subdir('slow')
subdir('lib')

tcp_inc = include_directories('include')

deps = [
  dependency('queue_dep'),
]

tcp_dep = declare_dependency(
  dependencies : deps,
  include_directories: tcp_inc,
)

meson.override_dependency('tcp_dep', tcp_dep)

statetool_inc = include_directories('include', 'slow/include')

statetool_deps = [
  dependency('cham'),
  dependency('tcp_config_dep'),
  dependency('tomgr_dep'),
]

executable(
  'statetool',
  files('statetool.c'),
  include_directories: statetool_inc,
  dependencies: statetool_deps,
)
