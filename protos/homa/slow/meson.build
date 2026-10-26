homa_slow = executable(
  'homa_slow',
  ['homa_slow.c', 'homa_init.c', 'homa_app.c', 'homa_timer.c',
   'homa_recovery.c'],
  include_directories: homa_inc,
  dependencies: homa_deps + [
    dependency('homa_config_dep'),
    dependency('log_dep'),
  ],
)
