config_inc = include_directories('include')

config_srcs = files('udp_config.c')

deps = [
  dependency('log_dep'),
]

config_lib = static_library(
  'udp_config',
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

meson.override_dependency('udp_config_dep', config_dep)
