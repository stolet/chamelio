# Build core utilities first (no interdependencies between them)
subdir('log')
subdir('uxsocket')
subdir('shm')
subdir('scheduler')
subdir('tomgr')
subdir('clock')

utils_inc = include_directories('include')

core_utils_deps = [
  dependency('log_dep'),
  dependency('uxsocket_dep'),
  dependency('shm_dep'),
  dependency('scheduler_dep'),
  dependency('clock_dep'),
  dependency('tomgr_dep'),
]

# Build extended utilities that may depend on core utils
subdir('pkt_hdrs')
subdir('log_pkt')
subdir('queue')

# Collect all static libraries to link into the main utils library
utils_libs = []

extended_utils_deps = [
  dependency('pkt_hdrs_dep'),
  dependency('log_pkt_dep'),
  dependency('queue_dep'),
]

# Create a shared library that links all utils libraries
utils_lib = library(
  'cham_utils',
  include_directories : utils_inc,
  link_whole : [ get_variable('log_lib'), 
                 get_variable('uxsocket_lib'),
                 get_variable('shm_lib'),
                 get_variable('scheduler_lib'),
                 get_variable('tomgr_lib'),
                 get_variable('clock_lib'),
                 get_variable('log_pkt_lib'),
                 get_variable('queue_lib')
               ],
  version : '1.0.0',
  install : true,
)

# Aggregate all utilities into a comprehensive dependency
all_utils_deps = core_utils_deps + extended_utils_deps

utils_dep = declare_dependency(
  link_with : utils_lib,
  include_directories : utils_inc,
  dependencies : all_utils_deps,
)

meson.override_dependency('cham_utils_dep', utils_dep)

# Install headers
install_headers(
  'include/utils.h',
  'queue/include/queue.h',
  'queue/include/queue_types.h',
  'queue/include/queue_fns.h',
  subdir: 'chamelio'
)

# Generate pkgconfig file
pkg = import('pkgconfig')
pkg.generate(
  utils_lib,
  description : 'Chamelio utilities library',
  subdirs : 'chamelio'
)