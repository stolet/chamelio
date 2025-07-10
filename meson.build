project('chameleo', 'c', 
  version : '0.1',
  default_options: [
    'c_std=gnu99',
    'optimization=3',
    'debug=true',
    'c_args=-march=native',
  ]
)

dpdk_dep = dependency('libdpdk', required : true)
threads_dep = dependency('threads')

subdir('utils')
subdir('chameleo')
