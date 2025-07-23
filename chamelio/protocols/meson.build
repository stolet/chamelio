protocols_inc = include_directories('include')

protocols_dep = declare_dependency(
  include_directories: protocols_inc,
)

meson.override_dependency('protocols_dep', protocols_dep)

subdir('ip')
subdir('eth')
subdir('udp')
