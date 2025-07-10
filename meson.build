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

subdir('utils')
subdir('chameleo')
