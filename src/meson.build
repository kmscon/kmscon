# SPDX-FileCopyrightText: 2022 Aetf <aetf@unlimited-code.works>
#
# SPDX-License-Identifier: MIT



#
# Binary File Compiler
#
embed_gen = generator(python,
  arguments: [
    meson.project_source_root() / 'tools' / 'embedded_file.py',
    '@EXTRA_ARGS@',
    '@INPUT@',
    '@BUILD_DIR@',
  ],
  output: ['@PLAINNAME@.bin.h', '@PLAINNAME@.bin.c'],
)
shader_regex = [
  '--regex', '^/\\*.*$',   '', # Start of multi-line comment
  '--regex', '^ \\*.*$',   '', # Multi-line comment body
  '--regex', '^[ \\t]*', '', # Indentation whitespace
  '--regex', '[\\r\\n]', '', # Newlines
]

src_inc = [include_directories('.')]

subdir('shl')

subdir('font')
subdir('input')
subdir('video')
subdir('render')

#
# libuterm
# The uterm library provides helpers to create terminals in user-space. They
# are not limited to text-based terminals but rather provide graphics contexts
# so arbitrary output can be displayed. Additionally, they provide VT
# abstractions and an input layer
#
uterm_srcs = [
  'uterm_monitor.c',
  'uterm_vt.c',
  'uterm_vt_fake.c',
  'uterm_vt_real.c',
]

uterm_dep = [
  libudev_deps,
  xkbcommon_deps,
  shl_deps,
  eloop_deps,
  input_deps,
  video_deps,
]

if enable_libseat
  uterm_srcs += 'uterm_vt_libseat.c'
  uterm_dep += libseat_deps
endif

uterm = static_library('uterm', uterm_srcs,
  dependencies: uterm_dep
)
uterm_deps = declare_dependency(
  link_with: [uterm],
  dependencies: uterm_dep
)

#
# Binaries
# These are the sources for the main binaries and test programs. They mostly
# consists of a single source file only and include all the libraries that are
# built as part of kmscon.
#

# conf as a standalone library for use in tests
conf = static_library('conf', 'conf.c', dependencies: [xkbcommon_deps])
conf_deps = declare_dependency(
  link_with: [conf],
  include_directories: [include_directories('.')]
)

kmscon_srcs = [
  'pty.c',
  'kmscon_seat.c',
  'kmscon_conf.c',
  'kmscon_issue.c',
  'kmscon_main.c',
]
kmscon_dep = [xkbcommon_deps,
  libtsm_deps,
  threads_deps,
  dl_deps,
  conf_deps,
  shl_deps,
  eloop_deps,
  uterm_deps,
  render_deps,
  font_deps,
]

if enable_session_dummy
  kmscon_srcs += 'kmscon_dummy.c'
endif
if enable_session_terminal
  kmscon_srcs += 'kmscon_terminal.c'
endif
if enable_dbus
  kmscon_srcs += 'dbus.c'
  kmscon_dep += dbus_deps
endif
kmscon = executable('kmscon', kmscon_srcs,
  dependencies: kmscon_dep,
  export_dynamic: true,
  install: true,
  install_dir: libexecdir,
)
