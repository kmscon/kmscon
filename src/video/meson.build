#
# Video
#

video_srcs = [
  'video.c',
]

video_dep = [
  shl_deps,
  eloop_deps,
]

if enable_video_fbdev
  video_srcs += [
    'fbdev_video.c',
    'fbdev_render.c'
  ]
endif

if enable_video_drm2d or enable_video_drm3d
  video_srcs += 'drm_shared.c'
  video_dep += libdrm_deps
endif

if enable_video_drm2d
  video_srcs += [
    'drm2d_video.c',
    'drm2d_render.c'
  ]
endif

video = static_library('video', video_srcs,
  dependencies: video_dep
)

video_deps = declare_dependency(
  link_with: [video],
  dependencies: video_dep
)

if enable_video_drm3d
  mod_drm3d = shared_module('mod-drm3d', [
    'drm3d_video.c',
    'drm3d_render.c',
    'mod_drm3d.c',
    embed_gen.process('drm3d_blend.vert', extra_args: shader_regex),
    embed_gen.process('drm3d_blend.frag', extra_args: shader_regex),
    ],
    name_prefix: '',
    dependencies: [video_deps, shl_deps, shl_gl_deps, libdrm_deps, egl_deps, gbm_deps, glesv2_deps],
    install: true,
    install_dir: moduledir,
  )
endif