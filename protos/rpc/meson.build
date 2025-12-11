subdir('fast')
subdir('slow')
subdir('lib')

rpc_inc = include_directories('include')

deps = [
  dependency('queue_dep'),
]

rpc_dep = declare_dependency(
  dependencies : deps,
  include_directories: rpc_inc,
)

meson.override_dependency('rpc_dep', rpc_dep)