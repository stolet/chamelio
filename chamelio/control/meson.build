control_inc = include_directories('include')

# TODO: add a pkgconfig file for all these libs in the dockerfile

bpftime_verif_inc = include_directories('/usr/local/include/bpftime-verifier')

llvmbpf_inc = include_directories('/usr/local/include/llvmbpf')
#'/opt/llvmbpf/include')

control_srcs = files('control.c', 'ivshmemif.c', 'guestif.c', 'ebpfif.cpp', 
  'verifierif.cpp')

llvmbpf_lib = cpp.find_library('llvmbpf', dirs: ['/usr/local/lib'], 
  required: true)

bpftime_verifier_lib = cpp.find_library('bpftime-verifier', 
  dirs: ['/usr/local/lib'], required: true)

bpftime_verif_dep = declare_dependency(
  dependencies: [bpftime_verifier_lib],
  include_directories: bpftime_verif_inc
)

llvmbpf_dep = declare_dependency(
  dependencies : [llvmbpf_lib],
  include_directories: llvmbpf_inc,
)

prevail_inc = include_directories('/usr/local/include/prevail')

prevail_lib = cpp.find_library('ebpfverifier',
  dirs: ['/usr/local/lib'],    
  required: true
)

prevail_dep = declare_dependency(
  include_directories: prevail_inc,
  dependencies: [prevail_lib]
)

spdlog_dep = cc.find_library('spdlog', dirs: ['/usr/local/lib'], required: true)

deps = [
  dependency('config_dep'),
  dependency('log_dep'),
  dependency('shm_dep'),
  dependency('queue_dep'),
  dependency('uxsocket_dep'),
  dependency('llvm'),
  llvmbpf_dep,
  prevail_dep,
  bpftime_verif_dep,
  spdlog_dep
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