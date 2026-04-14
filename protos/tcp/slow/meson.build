tcp_slow_inc = include_directories('.', 'include', '../include', 'app', 'rx',
  'state')

tcp_slow_srcs = files(
  'tcp_slow.c',
  'tcp_init.c',
  'state/tcp_sock_state.c',
  'state/tcp_listener.c',
  'state/tcp_flow.c',
  'state/tcp_ctl_tx.c',
  'app/tcp_app_req.c',
  'app/tcp_app_res.c',
  'app/tcp_appif.c',
  'rx/tcp_rx_ctl.c',
  'rx/tcp_rx_open.c',
  'rx/tcp_rx_close.c',
  'tcp_cc.c',
  'tcp_timeout.c',
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
