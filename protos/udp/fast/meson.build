udp_fast_inc = include_directories('include', '../include')

deps = [
  dependency('log_dep'),
  dependency('utils_dep'),
  dependency('pkt_hdrs_dep'),
  dependency('cham_lib'),
]

ecc = find_program('ecc', required: true)

# Compile udp_fast.ebpf.c to an object in the build dir
src = meson.project_source_root()

ebpf_incs = [
  src / 'utils/include',
  src / 'lib/include',
  src / 'utils/queue/include',
  src / 'utils/scheduler/include',
  src / 'utils/pkt_hdrs/include',
  src / 'protos/udp/include',
  src / 'protos/udp/fast/include',
]

ecc_inc_args = []
foreach d : ebpf_incs
  ecc_inc_args += ['-a=-I' + d]
endforeach

ebpf_obj = custom_target(
  'udp_fast_ebpf',
  input: 'udp_fast.ebpf.c',
  output: 'udp_fast_ebpf.o',
  command: [ ecc, '@INPUT@', '--output-path', meson.current_build_dir(),] + ecc_inc_args,
  build_by_default: true
)

udp_fast_lib = static_library(
  'udp_fast',
  'udp_fast.c',
  include_directories : udp_fast_inc,
  dependencies: deps,
  install : false,
)

udp_fast_dep = declare_dependency(
  link_with : udp_fast_lib,
  include_directories : udp_fast_inc,
  dependencies: deps,
)

meson.override_dependency('udp_fast_dep', udp_fast_dep)
