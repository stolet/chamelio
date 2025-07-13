project('chameleo', 'c', 
  version : '0.1',
  default_options: [
    'c_std=gnu99',
    'optimization=3',
    'debug=true',
    'c_args=-march=native',
  ]
)

# TODO: Maybe we need to link dpdk statically for better performance
dpdk_dep = dependency('libdpdk', required: true)
threads_dep = dependency('threads', required: true)

subdir('utils')
subdir('chameleo')