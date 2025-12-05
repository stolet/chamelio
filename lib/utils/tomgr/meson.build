tomgr_inc = include_directories('include', '../include')

tomgr_srcs = files('tomgr.c')

tomgr_lib = static_library(
  'tomgr',
  tomgr_srcs,
  include_directories : tomgr_inc,
  dependencies : deps,
  install : false,
)

tomgr_dep = declare_dependency(
  link_with : tomgr_lib,
  include_directories : tomgr_inc,
  dependencies : deps,
)

meson.override_dependency('tomgr_dep', tomgr_dep)