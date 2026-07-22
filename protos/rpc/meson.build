subdir('config')
subdir('fast')
subdir('slow')
subdir('lib')

rpc_inc = include_directories('include')

deps = [
  dependency('queue_dep'),
  dependency('cham'),
]

rpc_dep = declare_dependency(
  dependencies : deps,
  include_directories: rpc_inc,
)

meson.override_dependency('rpc_dep', rpc_dep)

statetool_inc = include_directories('include', 'slow/include')

statetool_deps = [
  dependency('cham'),
  dependency('rpc_config_dep'),
]

executable(
  'statetool',
  files('statetool.c'),
  include_directories: statetool_inc,
  dependencies: statetool_deps,
)