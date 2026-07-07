#
# libuterm
# Provides API for VT switching, and monitoring devices (input and gpu)
#
uterm_srcs = [
  'monitor.c',
  'vt.c',
  'vt_fake.c',
  'vt_real.c',
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
  uterm_srcs += 'vt_libseat.c'
  uterm_dep += libseat_deps
endif

uterm = static_library('uterm', uterm_srcs,
  dependencies: uterm_dep
)
uterm_deps = declare_dependency(
  link_with: [uterm],
  include_directories: [include_directories('.')],
  dependencies: uterm_dep
)
