homa_inc = include_directories('include')
homa_deps = [dependency('cham'), dependency('queue_dep')]

subdir('config')
subdir('fast')
subdir('slow')
subdir('lib')

alias_target('homa', [homa_fast, homa_slow, homa_lib])
