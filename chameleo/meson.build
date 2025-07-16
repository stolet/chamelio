subdir('config')
subdir('fast')
subdir('slow')
subdir('nic')
subdir('protocols')
subdir('queue')

sources_root = files('chameleo.c')

root_inc = include_directories('.', '../utils')

executable(
  'chameleo',
  sources_root,
  include_directories : root_inc,

  dependencies : [
    dpdk_dep,
    threads_dep,
  ],

  link_with : [
    config_lib,
    fast_lib,
    slow_lib,
    nic_lib,
    udp_lib,
    log_lib,
    queue_lib,
  ],
)
