nic_inc = include_directories('include')

nic_srcs = files('nic.c')

deps = [
  dependency('chamelio_dep'),
  dependency('config_dep'),
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
  include_directories : nic_inc,
)

meson.override_dependency('nic_dep', nic_dep)
