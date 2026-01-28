project('chamelio', ['c', 'cpp'],
  version : '1.0',
  default_options: [
    'c_std=gnu99',
    'cpp_std=c++20',
    'optimization=3',
    'debug=true',
    'c_args=-march=native -Wno-address-of-packed-member',
    'cpp_args=-march=native',
  ]
)

# TODO: Maybe we need to link dpdk statically for better performance
cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
dpdk_dep = dependency('libdpdk', required: true)
threads_dep = dependency('threads', required: true)
numa_dep = dependency('numa', required: true)
libbpf_dep  = dependency('libbpf', required: true)
rt_dep = cc.find_library('rt', required : true)


subdir('lib')
subdir('protos')
subdir('chamelio')
subdir('tests')