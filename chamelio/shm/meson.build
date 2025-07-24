shm_inc = include_directories('include')

shm_srcs = files('shmalloc.c', 'shm.c')

deps = [
  dependency('log_dep'),
]

shm_lib = static_library(
  'shm',
  shm_srcs,
  include_directories : shm_inc,
  dependencies : deps,
  install : false,
)

shm_dep = declare_dependency(
  link_with : shm_lib,
  include_directories : shm_inc,
)

meson.override_dependency('shm_dep', shm_dep)