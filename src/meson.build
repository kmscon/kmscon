# SPDX-FileCopyrightText: 2022 Aetf <aetf@unlimited-code.works>
#
# SPDX-License-Identifier: MIT

#
# Git-HEAD helper
#
githead = vcs_tag(
  input: 'shl_githead.c.in',
  output: 'shl_githead.c',
  command: ['git', 'describe', '--tags'],
  fallback: 'v' + meson.project_version(),
)

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
keymap_regex = [
  '--regex', '\\s*(=,)\\s*',       '\\1', # Whitespace around assignments and commas
  '--regex', '\\s*([\\]\\[])\\s*', '\\1', # Whitespace around []
  '--regex', '\\s*([{}])\\s*',     '\\1', # Whitespace around {}
  '--regex', '\\s*([()])\\s*',     '\\1', # Whitespace around ()
  '--regex', '^\\s*',              '',    # Indentation whitespace
  '--regex', '[\\r\\n]',           '',    # Newlines
  '--zero_term',                          # append an ASCII 0 so it can be used as regular C-string
]

#
# SHL - Static Helper Library
# The SHL subsystem contains several small code pieces used all over kmscon and
# other applications.
#
shl_srcs = [
  'shl_log.c',
  'shl_module.c',
  githead,
]
shl_dep = [
  htable_deps,
  xkbcommon_deps
]

shl = static_library('shl', shl_srcs, dependencies: shl_dep)
shl_deps = declare_dependency(
  link_with: [shl],
  include_directories: [include_directories('.')],
  dependencies: shl_dep
)

if enable_glesv2
  shl_gl_srcs = [
    'shl_gl_shader.c',
    'shl_gl_math.c',
  ]
  shl_gl_dep = [
    shl_deps,
    glesv2_deps
  ]

shl_gl = static_library('shl_gl', shl_gl_srcs, dependencies: shl_gl_dep)
shl_gl_deps = declare_dependency(
  link_with: [shl_gl],
  include_directories: [include_directories('.')],
  dependencies: shl_gl_dep
)
endif

#
# libeloop
# This library contains the whole event-loop implementation of kmscon. It is
# compiled into a separate object to allow using it in several other programs.
#
eloop = static_library('eloop', 'eloop.c', dependencies: [shl_deps])
eloop_deps = declare_dependency(
  link_with: [eloop],
)

#
# libuterm
# The uterm library provides helpers to create terminals in user-space. They
# are not limited to text-based terminals but rather provide graphics contexts
# so arbitrary output can be displayed. Additionally, they provide VT
# abstractions and an input layer
#
uterm_srcs = [
  'uterm_video.c',
  'uterm_monitor.c',
  'uterm_vt.c',
  'uterm_input.c',
  'uterm_input_uxkb.c',
  'uterm_input_pointer.c',
  embed_gen.process('uterm_input_fallback.xkb', extra_args: keymap_regex),
]
uterm_dep = [
  libudev_deps,
  xkbcommon_deps,
  shl_deps,
  eloop_deps,
]

if enable_multi_seat
  uterm_srcs += 'uterm_systemd.c'
  uterm_dep += libsystemd_deps
endif
if enable_video_fbdev
  uterm_srcs += [
    'uterm_fbdev_video.c',
    'uterm_fbdev_render.c'
  ]
endif
if enable_video_drm2d or enable_video_drm3d
  uterm_srcs += 'uterm_drm_shared.c'
  uterm_dep += libdrm_deps
endif
if enable_video_drm2d
  uterm_srcs += [
    'uterm_drm2d_video.c',
    'uterm_drm2d_render.c'
  ]
endif
uterm = static_library('uterm', uterm_srcs,
  dependencies: uterm_dep
)
uterm_deps = declare_dependency(
  link_with: [uterm],
  dependencies: uterm_dep
)

if enable_video_drm3d
  mod_drm3d = shared_module('mod-drm3d', [
    'uterm_drm3d_video.c',
    'uterm_drm3d_render.c',
    'uterm_mod_drm3d.c',
    embed_gen.process('uterm_drm3d_blend.vert', extra_args: shader_regex),
    embed_gen.process('uterm_drm3d_blend.frag', extra_args: shader_regex),
    embed_gen.process('uterm_drm3d_fill.vert', extra_args: shader_regex),
    embed_gen.process('uterm_drm3d_fill.frag', extra_args: shader_regex),
    ],
    name_prefix: '',
    dependencies: [uterm_deps, shl_deps, shl_gl_deps, libdrm_deps, egl_deps, gbm_deps, glesv2_deps],
    install: true,
    install_dir: moduledir,
  )
endif

#
# Unifont Generator
# This generates the unifont sources from raw hex-encoded font data.
#
genunifont = executable('genunifont', 'genunifont.c', native: true)

unifont_bin = custom_target('unifont-bin',
  input: ['font_unifont_data.hex'],
  output: ['font_unifont_data'],
  command: [genunifont, '@OUTPUT@', '@INPUT@']
)

#
# Kmscon Modules
#
if enable_font_unifont
  mod_unifont = shared_module('mod-unifont', [
      'font_unifont.c',
      'kmscon_mod_unifont.c',
      embed_gen.process(unifont_bin),
    ],
    name_prefix: '',
    dependencies: [libtsm_deps, shl_deps],
    install: true,
    install_dir: moduledir,
  )
endif

if enable_font_freetype
  mod_freetype = shared_module('mod-freetype', [
      'font_freetype.c',
      'kmscon_mod_freetype.c',
    ],
    name_prefix: '',
    dependencies: [libtsm_deps, freetype_deps, fontconfig_deps, threads_deps, shl_deps],
    install: true,
    install_dir: moduledir,
  )
endif

if enable_font_pango
  mod_pango = shared_module('mod-pango', [
      'font_pango.c',
      'kmscon_mod_pango.c',
    ],
    name_prefix: '',
    dependencies: [libtsm_deps, pango_deps, threads_deps, shl_deps],
    install: true,
    install_dir: moduledir,
  )
endif

if enable_renderer_gltex
  mod_gltex = shared_module('mod-gltex', [
      'text_gltex.c',
      'kmscon_mod_gltex.c',
      embed_gen.process('text_gltex_atlas.vert', extra_args: shader_regex),
      embed_gen.process('text_gltex_atlas.frag', extra_args: shader_regex),
    ],
    name_prefix: '',
    dependencies: [libtsm_deps, glesv2_deps, shl_deps, shl_gl_deps],
    install: true,
    install_dir: moduledir,
  )
endif

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
  'font.c',
  'font_rotate.c',
  'font_8x16.c',
  'text.c',
  'text_bbulk.c',
  'kmscon_seat.c',
  'kmscon_conf.c',
  'kmscon_main.c',
]
if enable_session_dummy
  kmscon_srcs += 'kmscon_dummy.c'
endif
if enable_session_terminal
  kmscon_srcs += 'kmscon_terminal.c'
endif
kmscon = executable('kmscon', kmscon_srcs,
  dependencies: [xkbcommon_deps, libtsm_deps, threads_deps, dl_deps, conf_deps, shl_deps, eloop_deps, uterm_deps],
  export_dynamic: true,
  install: true,
  install_dir: libexecdir,
)
