netvirt_inc = include_directories('include')

netvirt_srcs = files('netvirt_table.c', 'netvirt_parser.c')

deps = [
  dependency('log_dep'),
]

netvirt_lib = static_library(
  'netvirt',
  netvirt_srcs,
  include_directories : netvirt_inc,
  dependencies : deps,
  install : false,
)

netvirt_dep = declare_dependency(
  link_with : netvirt_lib,
  dependencies: deps,
  include_directories : netvirt_inc,
)

meson.override_dependency('netvirt_dep', netvirt_dep)
