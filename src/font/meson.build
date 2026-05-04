
font_srcs = [
  'font.c',
  'font_8x16.c',
   embed_gen.process('font_8x16.data'),
]

font_dep = [
  shl_deps,
]

font = static_library('input', font_srcs,
  dependencies: font_dep
)

font_deps = declare_dependency(
  link_with: [font],
  dependencies: font_dep
)

#
# Unifont Generator
# This generates the unifont sources from raw hex-encoded font data.
#
genunifont = executable('genunifont', 'genunifont.c', dependencies: [zlib_deps], native: true)

unifont_bin = custom_target('unifont-bin',
  input: ['font_unifont_data.hex'],
  output: ['font_unifont_data'],
  command: [genunifont, '@OUTPUT@', '@INPUT@']
)

#
# Fonts Modules
#
if enable_font_unifont
  mod_unifont = shared_module('mod-unifont', [
      'font_unifont.c',
      'mod_unifont.c',
      embed_gen.process(unifont_bin),
    ],
    name_prefix: '',
    dependencies: [libtsm_deps, shl_deps, zlib_deps],
    install: true,
    install_dir: moduledir,
  )
endif

if enable_font_freetype
  mod_freetype = shared_module('mod-freetype', [
      'font_freetype.c',
      'mod_freetype.c',
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
      'mod_pango.c',
    ],
    name_prefix: '',
    dependencies: [libtsm_deps, pango_deps, threads_deps, shl_deps],
    install: true,
    install_dir: moduledir,
  )
endif