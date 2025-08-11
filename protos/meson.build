subdir('udp')

protos_lib = static_library(
  'protos',
  install : false,
)

deps = [
  dependency('udp_dep'),
]

protos_dep = declare_dependency(
  link_with : protos_lib,
  dependencies : deps,
)

meson.override_dependency('protos_dep', protos_dep)