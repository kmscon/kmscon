# Text renderer

render_srcs = [
  'text.c',
  'bbulk.c',
]

render_dep = [
  shl_deps,
  libtsm_deps,
  video_deps,
]

render = static_library('render', render_srcs,
  dependencies: render_dep,
  include_directories: src_inc,
)

render_deps = declare_dependency(
  link_with: [render],
  dependencies: render_dep
)

if enable_renderer_gltex
  mod_gltex = shared_module('mod-gltex', [
      'gltex.c',
      'mod_gltex.c',
      embed_gen.process('gltex_atlas.vert', extra_args: shader_regex),
      embed_gen.process('gltex_atlas.frag', extra_args: shader_regex),
    ],
    name_prefix: '',
    dependencies: [libtsm_deps, glesv2_deps, shl_deps, shl_gl_deps],
    include_directories: src_inc,
    install: true,
    install_dir: moduledir,
  )
endif