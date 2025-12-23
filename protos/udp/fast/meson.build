ecc = find_program('ecc', required: true)

# Compile udp_fast.ebpf.c to an object in the build dir
src = meson.project_source_root()

ebpf_incs = [
  src / 'lib/utils/include',
  src / 'lib/include',
  src / 'lib/utils/queue/include',
  src / 'lib/utils/scheduler/include',
  src / 'lib/utils/pkt_hdrs/include',
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
