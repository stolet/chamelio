ecc = find_program('ecc', required: true)
src = meson.project_source_root()
ebpf_incs = [
  src / 'lib/include',
  src / 'lib/utils/include',
  src / 'lib/utils/queue/include',
  src / 'lib/utils/scheduler/include',
  src / 'lib/utils/pkt_hdrs/include',
]
ecc_inc_args = []
foreach d : ebpf_incs
  ecc_inc_args += ['-a=-I' + d]
endforeach

homa_fast = custom_target(
  'homa_fast_ebpf',
  input: 'homa_fast.ebpf.c',
  output: 'homa_fast.bpf.o',
  command: [ecc, '@INPUT@', '--output-path', meson.current_build_dir()]
    + ecc_inc_args,
  build_by_default: true,
)
