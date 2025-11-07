test_cham_lib_srcs = files('test_cham_lib.c','test_utils.c')
test_cham_scheduler_srcs = files('test_cham_scheduler.c', 'test_utils.c')
test_cham_queue_srcs = files('test_cham_queue.c', 'test_utils.c')

deps = [
  dependency('cham_lib'),
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

test_cham_lib = executable(
  'test_cham_lib',
  test_cham_lib_srcs,
  dependencies: deps,
)

test('cham-lib-test', test_cham_lib, depends: ebpf_obj)

test_cham_scheduler = executable(
  'test_cham_scheduler',
  test_cham_scheduler_srcs,
  dependencies: deps,
)

test('cham-scheduler-test', test_cham_scheduler, depends: ebpf_obj)

test_cham_queue = executable(
  'test_cham_queue',
  test_cham_queue_srcs,
  dependencies: deps,
)

test('cham-queue-test', test_cham_queue, depends: ebpf_obj)
