rpc_slow_inc = include_directories('include', '../include')

rpc_slow_srcs = files('rpc_slow.c', 'appif.c')

deps = [
  dependency('cham'),
  dependency('log_dep'),
  dependency('rpc_config_dep'),
]

executable(
  'rpc_slow',
  rpc_slow_srcs,
  include_directories: rpc_slow_inc,
  dependencies: deps,
)
