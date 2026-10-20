homa_inc = include_directories('include')
homa_deps = [dependency('cham'), dependency('queue_dep')]

homa_dep = declare_dependency(
  dependencies: homa_deps,
  include_directories: homa_inc,
)
meson.override_dependency('homa_dep', homa_dep)

subdir('config')
subdir('fast')
subdir('slow')
subdir('lib')

alias_target('homa', [homa_fast, homa_slow, homa_lib])
