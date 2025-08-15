project('chamelio', 'c', 
  version : '1.0',
  default_options: [
    'c_std=gnu99',
    'optimization=3',
    'debug=true',
    'c_args=-march=native -Wno-address-of-packed-member',
  ]
)

# TODO: Maybe we need to link dpdk statically for better performance
cc = meson.get_compiler('c')
dpdk_dep = dependency('libdpdk', required: true)
threads_dep = dependency('threads', required: true)
rt_dep = cc.find_library('rt', required : true)

subdir('utils')
subdir('chamelio')
subdir('lib')
subdir('protos')
subdir('examples')