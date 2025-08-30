test_srcs = files('test_chamelio.c','test_utils.c')

deps = [
  dependency('chamelio_lib'),
]

test_chamelio = executable(
  'test_chamelio',
  test_srcs,
  dependencies: deps,
)

test('chamelio-test', test_chamelio)
