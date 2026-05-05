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

ebpf_deps = files(
  '../../../lib/include/cham_fast.h',
  '../../../lib/utils/include/utils.h',
  '../../../lib/utils/pkt_hdrs/include/ip_hdr.h',
  '../../../lib/utils/pkt_hdrs/include/udp_hdr.h',
  '../../../lib/utils/queue/include/queue.h',
  '../../../lib/utils/queue/include/queue_types.h',
  '../../../lib/utils/scheduler/include/scheduler.h',
  '../include/udp.h',
  '../include/udp_queue_types.h',
)

ecc_inc_args = []
foreach d : ebpf_incs
  ecc_inc_args += ['-a=-I' + d]
endforeach

ebpf_srcs = [
  'udp_fast.ebpf.c',
  'udp_fast_64.ebpf.c',
  'udp_fast_128.ebpf.c',
  'udp_fast_256.ebpf.c',
  'udp_fast_512.ebpf.c',
  'udp_fast_1024.ebpf.c',
]

udp_ebpf_objs = []
foreach ebpf_src : ebpf_srcs
  ebpf_name = ebpf_src.split('.')[0]
  udp_ebpf_objs += custom_target(
    ebpf_name + '_ebpf',
    input: ebpf_src,
    output: ebpf_name + '.bpf.o',
    command: [ ecc, '@INPUT@', '--output-path', meson.current_build_dir(),] + ecc_inc_args,
    depend_files: ebpf_deps,
    build_by_default: true
  )
endforeach
