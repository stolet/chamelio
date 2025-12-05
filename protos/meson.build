subdir('udp')

deps = [
  dependency('udp_dep'),
]

protos_dep = declare_dependency(
  dependencies : deps,
)

meson.override_dependency('protos_dep', protos_dep)