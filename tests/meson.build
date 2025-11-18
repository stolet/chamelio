test_lib_srcs = files('test_lib.c','test_utils.c')
test_scheduler_srcs = files('test_scheduler.c', 'test_utils.c')
test_queue_srcs = files('test_queue.c', 'test_utils.c')
test_arp_srcs = files('test_arp.c', 'test_utils.c')

deps = [
  dependency('cham_lib'),
  dependency('queue_dep'),
  dependency('arp_dep'),
  dependency('scheduler_dep')
]

# Find ecc compiler
ecc = find_program('ecc', required: true)

# Compile sample.ebpf.c to an object in the build dir
ebpf_obj = custom_target(
  'sample_ebpf',
  input: 'sample.ebpf.c',
  output: 'sample_ebpf.o',
  command: [ecc, '@INPUT@', '--output-path', meson.current_build_dir()],
  build_by_default: true
)

test_lib = executable(
  'test_lib',
  test_lib_srcs,
  dependencies: deps,
)

test('lib-test', test_lib, depends: ebpf_obj)

test_scheduler = executable(
  'test_scheduler',
  test_scheduler_srcs,
  dependencies: deps,
)

test('scheduler-test', test_scheduler, depends: ebpf_obj)

test_queue = executable(
  'test_queue',
  test_queue_srcs,
  dependencies: deps,
)

test('queue-test', test_queue, depends: ebpf_obj)

test_arp = executable(
  'test_arp',
  test_arp_srcs,
  dependencies: deps,
)

test('arp-test', test_arp)
