config_inc = include_directories('include', '../include')

config_srcs = files('rpc_config.c')

deps = [
  dependency('cham'),
  dependency('log_dep'),
]

config_lib = static_library(
  'rpc_config',
  config_srcs,
  include_directories : config_inc,
  dependencies : deps,
  install : false,
)

config_dep = declare_dependency(
  link_with : config_lib,
  include_directories : config_inc,
  dependencies: deps,
)

meson.override_dependency('rpc_config_dep', config_dep)
