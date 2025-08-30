test_cham_lib_srcs = files('test_cham_lib.c','test_utils.c')
test_cham_scheduler_srcs = files('test_cham_scheduler.c', 'test_utils.c')

deps = [
  dependency('cham_lib'),
]

test_cham_lib = executable(
  'test_cham_lib',
  test_cham_lib_srcs,
  dependencies: deps,
)

test('cham-lib-test', test_cham_lib)

test_cham_scheduler = executable(
  'test_cham_scheduler',
  test_cham_scheduler_srcs,
  dependencies: deps,
)

test('cham-scheduler-test', test_cham_scheduler)
