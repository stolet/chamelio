control_inc = include_directories('include')
chamelio_inc = include_directories('../include')
llvmbpf_inc = include_directories('/usr/local/include/llvmbpf')

control_srcs = files('control.c', 'ivshmemif.c', 'guestif.c')

llvm_dep = dependency(
  'llvm',
  required : true
)

llvmbpf_dep = cc.find_library(
  'llvmbpf_vm',
  dirs : ['/usr/local/lib'],
  required : true
)

spdlog_dep  = cc.find_library('spdlog', dirs: ['/usr/local/lib'], required: true)

deps = [
  dependency('config_dep'),
  dependency('nic_dep'),
  dependency('log_dep'),
  dependency('shm_dep'),
  dependency('arp_dep'),
  dependency('queue_dep'),
  dependency('uxsocket_dep'),
  dependency('cham_lib'),
  llvm_dep,
  llvmbpf_dep,
  libbpf_dep,
  spdlog_dep,
]

control_lib = static_library(
  'control',
  control_srcs,
  include_directories : [control_inc, llvmbpf_inc, chamelio_inc],
  dependencies : deps,
  install : false,
)

control_dep = declare_dependency(
  link_with: control_lib,
  include_directories: [control_inc, llvmbpf_inc],
)

meson.override_dependency('control_dep', control_dep)