tcp_slow_inc = include_directories('include', '../include')

tcp_slow_srcs = files(
  'tcp_slow.c',
  'tcp_slow_init.c',
  'tcp_slow_state.c',
  'tcp_slow_app.c',
  'tcp_slow_fast.c',
  'tcp_slow_timeout.c',
  'appif.c',
)

deps = [
  dependency('cham'),
  dependency('log_dep'),
  dependency('clock_dep'),
  dependency('tcp_config_dep'),
  dependency('tomgr_dep'),
]

executable(
  'tcp_slow',
  tcp_slow_srcs,
  include_directories: tcp_slow_inc,
  dependencies: deps,
)
