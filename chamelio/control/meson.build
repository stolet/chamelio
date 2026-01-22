control_inc = include_directories('include')
chamelio_inc = include_directories('../include')
llvmbpf_inc = include_directories('/usr/local/include/llvmbpf')
prevail_inc = include_directories('/usr/local/include/prevail')
llvm_config = find_program('llvm-config', required: true)
llvm_inc = run_command(llvm_config, '--includedir', check: true).stdout().strip()
llvm_lib = run_command(llvm_config, '--libdir', check: true).stdout().strip()

control_srcs = files('control.c',
    'ivshmemif.c', 'guestif.c',
    'verifier.cpp', 'clang.cpp')

build_dir_define = '-DCHAMELIO_BUILD_DIR="' + meson.project_build_root() + '"'
src_dir_define = '-DCHAMELIO_SRC_DIR="' + meson.project_source_root() + '"'
control_c_args = [build_dir_define, src_dir_define]
control_cpp_args = [build_dir_define, src_dir_define]

prevail_lib = cc.find_library('prevail',
  dirs: ['/usr/local/lib'],
  required: true
)

prevail_dep = declare_dependency(
  include_directories: prevail_inc,
  dependencies: [prevail_lib]
)

llvm_dep = dependency(
  'llvm',
  required : true
)

clang_cpp_lib = cpp.find_library('clang-cpp', dirs: [llvm_lib], required: true)
clang_cpp_dep = declare_dependency(
  include_directories: include_directories(llvm_inc),
  dependencies: [clang_cpp_lib]
)

llvmbpf_dep = cc.find_library(
  'llvmbpf_vm',
  dirs : ['/usr/local/lib'],
  required : true
)

spdlog_dep  = cc.find_library('spdlog', dirs: ['/usr/local/lib'], required: true)

deps = [
  dependency('cham_config_dep'),
  dependency('nic_dep'),
  dependency('log_dep'),
  dependency('shm_dep'),
  dependency('arp_dep'),
  dependency('queue_dep'),
  dependency('uxsocket_dep'),
  dependency('tomgr_dep'),
  dependency('clock_dep'),
  dependency('netvirt_dep'),
  dependency('cham'),
  llvm_dep,
  llvmbpf_dep,
  libbpf_dep,
  spdlog_dep,
  prevail_dep,
  clang_cpp_dep,
]

control_lib = static_library(
  'control',
  control_srcs,
  include_directories : [control_inc, llvmbpf_inc, chamelio_inc],
  c_args : control_c_args,
  cpp_args : control_cpp_args,
  dependencies : deps,
  install : false,
)

control_dep = declare_dependency(
  link_with: control_lib,
  include_directories: [control_inc, llvmbpf_inc],
  dependencies: deps,
)

meson.override_dependency('control_dep', control_dep)
