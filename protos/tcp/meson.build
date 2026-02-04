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