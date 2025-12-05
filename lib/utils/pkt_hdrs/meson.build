pkt_hdrs_inc = include_directories('include', '../include')

deps = [
]

pkt_hdrs_dep = declare_dependency(
  include_directories : pkt_hdrs_inc,
  dependencies : deps,
)

meson.override_dependency('pkt_hdrs_dep', pkt_hdrs_dep)
