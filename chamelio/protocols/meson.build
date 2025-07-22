subdir('udp')
subdir('eth')
subdir('ip')

protocols_lib = static_library(
  'protocols',
  'shm.c',
  include_directories : [
    include_directories('..', '../..')
  ]
)