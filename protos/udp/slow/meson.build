udp_slow_inc = include_directories('include', '../include')

udp_slow_srcs = files('udp_slow.c', 'appif.c')

deps = [
  dependency('cham'),
  dependency('log_dep'),
]

executable(
  'udp_slow',
  udp_slow_srcs,
  include_directories: udp_slow_inc,
  dependencies: deps,
)
