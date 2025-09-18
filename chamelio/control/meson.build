control_inc = include_directories('include')
llvmbpf_inc = include_directories(
  '/opt/llvmbpf/include',
  '/usr/include/llvmbpf',
  )

control_srcs = files('control.c', 'ivshmemif.c', 'guestif.c', 'ebpfif.cpp')

llvmbpf_libdirs = [
  '/opt/llvmbpf/build',
  '/opt/llvmbpf/build/lib',
]

llvmbpf_lib = cpp.find_library('llvmbpf', dirs: llvmbpf_libdirs, required: false)

llvmbpf_dep = declare_dependency(
  dependencies : [llvmbpf_lib],
  include_directories: llvmbpf_inc,
)

deps = [
  dependency('config_dep'),
  dependency('log_dep'),
  dependency('shm_dep'),
  dependency('queue_dep'),
  dependency('uxsocket_dep'),
  llvmbpf_dep,
]

control_lib = static_library(
  'control',
  control_srcs,
  include_directories : control_inc,
  dependencies : deps,
  install : false,
)

control_dep = declare_dependency(
  link_with: control_lib,
  include_directories: control_inc,
)

meson.override_dependency('control_dep', control_dep)