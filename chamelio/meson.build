chamelio_inc = include_directories('include')

chamelio_srcs = files('chamelio.c', 'ebpf.cpp')

chamelio_dep = declare_dependency(
  include_directories: chamelio_inc
)

meson.override_dependency('chamelio_dep', chamelio_dep)

subdir('config')
subdir('nic')
subdir('arp')
subdir('fast')
subdir('control')

deps = [
  dependency('cham_utils_dep'),
  dependency('config_dep'),
  dependency('shm_dep'),
  dependency('queue_dep'),
  dependency('nic_dep'),
  dependency('arp_dep'),
  dependency('fast_dep'),
  dependency('control_dep'),
  dpdk_dep,
  threads_dep,
  rt_dep,
]

executable(
  'chamelio',
  chamelio_srcs,
  include_directories: chamelio_inc,
  dependencies : deps
)
