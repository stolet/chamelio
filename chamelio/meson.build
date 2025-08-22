chamelio_inc = include_directories('include')

chamelio_srcs = files('chamelio.c')

chamelio_dep = declare_dependency(
  include_directories: chamelio_inc
)

meson.override_dependency('chamelio_dep', chamelio_dep)

subdir('config')
subdir('protocols')
subdir('nic')
subdir('fast')
subdir('control')

deps = [
  dependency('utils_dep'),
  dependency('config_dep'),
  dependency('shm_dep'),
  dependency('queue_dep'),
  dependency('udp_old_dep'),
  dependency('ip_dep'),
  dependency('eth_dep'),
  dependency('nic_dep'),
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
