config_inc = include_directories('include')

config_srcs = files('homa_config.c')

config_lib = static_library(
  'homa_config',
  config_srcs,
  include_directories : [config_inc, homa_inc],
  install : false,
)

config_dep = declare_dependency(
  link_with : config_lib,
  include_directories : config_inc,
)

meson.override_dependency('homa_config_dep', config_dep)
