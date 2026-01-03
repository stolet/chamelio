nic_inc = include_directories('include')

nic_srcs = files('nic.c', 'nic_fast.c')

deps = [
  dependency('chamelio_dep'),
  dependency('cham_config_dep'),
  dependency('log_dep'),
]

nic_lib = static_library(
  'nic',
  nic_srcs,
  include_directories : nic_inc,
  dependencies : deps,
  install : false,
)

nic_dep = declare_dependency(
  link_with : nic_lib,
  dependencies: deps,
  include_directories : nic_inc,
)

meson.override_dependency('nic_dep', nic_dep)
