eth_inc = include_directories('include')

deps = [
  dependency('utils_dep'),
  dependency('log_dep'),
  dependency('protocols_dep'),
]

eth_lib = static_library(
  'eth',
  include_directories : eth_inc,
  dependencies: deps,
  install : false,
)

eth_dep = declare_dependency(
  link_with : eth_lib,
  include_directories : eth_inc,
  dependencies: deps,
)

meson.override_dependency('eth_dep', eth_dep)