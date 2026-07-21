# misc as a standalone library for use in tests

misc_srcs = [
  'asciinema.c',
  'conf.c',
  'issue.c',
  'issue_network.c',
  'pty.c',
]
misc_deps = [xkbcommon_deps, shl_deps, libtsm_deps]

if enable_dbus
  misc_srcs += 'dbus.c'
  misc_deps += dbus_deps
endif

misc_inc = include_directories('.')
misc_dep = static_library('misc', misc_srcs, dependencies: [misc_deps])
misc_deps = declare_dependency(
  link_with: [misc_dep],
  include_directories: [misc_inc]
)
