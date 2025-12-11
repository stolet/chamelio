subdir('udp')
subdir('rpc')

deps = [
  dependency('udp_dep'),
  dependency('rpc_dep'),
]

protos_dep = declare_dependency(
  dependencies : deps,
)

meson.override_dependency('protos_dep', protos_dep)