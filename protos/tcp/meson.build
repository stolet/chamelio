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

tracetool_inc = include_directories('include', 'slow/include')

tracetool_deps = [
  dependency('cham'),
  dependency('tcp_config_dep'),
  dependency('tomgr_dep'),
]

executable(
  'tracetool',
  files('tracetool.c'),
  include_directories: tracetool_inc,
  dependencies: tracetool_deps,
)
