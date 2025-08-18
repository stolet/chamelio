udp_slow_inc = include_directories('include')

udp_slow_srcs = files('udp_slow.c', 'appif.c')

deps = [
  dependency('chamelio_lib'),
  dependency('log_dep'),
  dependency('udp_queue_dep'),
]

executable(
  'udp_slow',
  udp_slow_srcs,
  include_directories: udp_slow_inc,
  dependencies: deps,
)
