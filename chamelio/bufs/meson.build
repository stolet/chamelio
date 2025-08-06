bufs_inc = include_directories('include')

bufs_srcs = files('bufs.c')

deps = []

bufs_lib = static_library(
  'bufs',
  bufs_srcs,
  include_directories : bufs_inc,
  dependencies : deps,
  install : false,
)

bufs_dep = declare_dependency(
  link_with : bufs_lib,
  dependencies: deps,
  include_directories : bufs_inc,
)

meson.override_dependency('bufs_dep', bufs_dep)
