subdir('config')
subdir('protocols')
subdir('fast')
subdir('slow')
subdir('nic')
subdir('queue')

sources_root = files('chamelio.c')

root_inc = include_directories('.', '../utils')

executable(
  'chamelio',
  sources_root,
  include_directories : root_inc,

  dependencies : [
    dpdk_dep,
    threads_dep,
    rt_dep,
  ],

  link_with : [
    config_lib,
    udp_lib,
    protocols_lib,
    fast_lib,
    slow_lib,
    nic_lib,
    log_lib,
    queue_lib,
  ],
)
