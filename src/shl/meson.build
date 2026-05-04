#
# SHL - Static Helper Library
# The SHL subsystem contains several small code pieces used all over kmscon and
# other applications.
#

# Git-HEAD helper
githead = vcs_tag(
  input: 'githead.c.in',
  output: 'githead.c',
  command: ['git', 'describe', '--tags'],
  fallback: 'v' + meson.project_version(),
)

shl_srcs = [
  'log.c',
  'module.c',
  githead,
]
shl_dep = [
  htable_deps,
  xkbcommon_deps
]

shl = static_library('shl', shl_srcs, dependencies: shl_dep)
shl_deps = declare_dependency(
  link_with: [shl],
  dependencies: shl_dep,
  include_directories: src_inc,
)

#
# libeloop
# This library contains the whole event-loop implementation of kmscon. It is
# compiled into a separate object to allow using it in several other programs.
#
eloop = static_library('eloop', 'eloop.c', dependencies: [shl_deps])
eloop_deps = declare_dependency(
  link_with: [eloop],
)

if enable_glesv2
  shl_gl_srcs = [
    'gl_shader.c',
  ]
  shl_gl_dep = [
    shl_deps,
    glesv2_deps
  ]

shl_gl = static_library('shl_gl', shl_gl_srcs, dependencies: shl_gl_dep)
shl_gl_deps = declare_dependency(
  link_with: [shl_gl],
  dependencies: shl_gl_dep
)
endif


