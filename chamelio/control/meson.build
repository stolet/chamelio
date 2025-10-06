control_inc = include_directories('include')

# TODO: add a pkgconfig file for all these libs in the dockerfile

bpftime_verif_inc = include_directories('/usr/local/include/bpftime-verifier')

llvmbpf_inc = include_directories(
  '/opt/llvmbpf/include',
  '/usr/include/llvmbpf',
  )

control_srcs = files('control.c', 'ivshmemif.c', 'guestif.c', 'ebpfif.cpp', 
  'verifierif.cpp')

llvmbpf_libdirs = [
  '/opt/llvmbpf/build',
  '/opt/llvmbpf/build/lib',
]

llvmbpf_lib = cpp.find_library('llvmbpf', dirs: llvmbpf_libdirs, 
  required: false)

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

prevail = '/bpftime/src/bpftime-verifier/ebpf-verifier'

prevail_inc = include_directories(
  prevail,             
  prevail + '/src',      
  prevail + '/src/crab'   
)

prevail_lib = cpp.find_library('ebpfverifier',
  dirs: ['/usr/local/lib'],    
  required: true
)

prevail_dep = declare_dependency(
  include_directories: prevail_inc,
  dependencies: [prevail_lib]
)

deps = [
  dependency('config_dep'),
  dependency('log_dep'),
  dependency('shm_dep'),
  dependency('queue_dep'),
  dependency('uxsocket_dep'),
  llvmbpf_dep,
  prevail_dep,
  bpftime_verif_dep
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