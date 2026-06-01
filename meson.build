# SPDX-FileCopyrightText: 2022 Aetf <aetf@unlimited-code.works>
#
# SPDX-License-Identifier: MIT

project('kmscon', 'c',
  version: '10.0.0',
  license: 'MIT',
  # meson 0.58: f-string
  # meson 0.62: dependency libdl
  meson_version: '>=1.1',
  default_options: [
    'warning_level=1',
    'werror=true',
    'buildtype=debugoptimized',
    'c_std=gnu99',
  ]
)

add_project_arguments(
  '-ffast-math',
  '-fno-strict-aliasing',
  '-ffunction-sections',
  '-fdata-sections',
  '-fstack-protector',
  '-fvisibility=hidden',
  '-D_GNU_SOURCE',
  '-D_POSIX_C_SOURCE=200809L',
  language: 'c'
)

#
# Directory structure
#
prefix = get_option('prefix')
bindir = get_option('bindir')
datadir= get_option('datadir')
sysconfdir = get_option('sysconfdir') / meson.project_name()
libexecdir = get_option('libexecdir') / meson.project_name()
mandir = get_option('mandir')
moduledir = get_option('libdir') / meson.project_name()

systemd_deps = dependency('systemd', required: false)
systemdsystemunitdir = systemd_deps.get_variable('systemdsystemunitdir', default_value: 'lib/systemd/system')

#
# Required dependencies
#
fs = import('fs')

xkbcommon_deps = dependency('xkbcommon', version: '>=0.5.0')
libtsm_deps = dependency('libtsm', version: '>=4.5.0', allow_fallback: true)
libudev_deps = dependency('libudev', version: '>=172')
dl_deps = dependency('dl')
threads_deps = dependency('threads')
zlib_deps = dependency('zlib')
zlib_deps_native = dependency('zlib', native: true)

python = find_program('python3')

#
# Dependencies
# These are controlled by feature options.
# When the feature is disabled, the corresponding dependency is not searched,
# instad, a disabler object is produced to disable anything uses it.
# This also allows early reporting when a feature is enabled but its dependencis
# are not found.
#
require_glesv2 = get_option('video_drm3d').enabled() or get_option('renderer_gltex').enabled()
require_libdrm = get_option('video_drm2d').enabled() or get_option('video_drm3d').enabled()

libseat_deps = dependency('libseat', disabler: true, required: get_option('libseat'))
libdrm_deps = dependency('libdrm', disabler: true, required: require_libdrm)
gbm_deps = dependency('gbm', disabler: true, required: get_option('video_drm3d'))
egl_deps = dependency('egl', disabler: true, required: get_option('video_drm3d'))
glesv2_deps = dependency('glesv2', disabler: true, required: require_glesv2)
pango_deps = dependency('pangoft2', disabler: true, required: get_option('font_pango'))
freetype_deps = dependency('freetype2', disabler: true, required: get_option('font_freetype'))
fontconfig_deps = dependency('fontconfig', disabler: true, required: get_option('font_freetype'))
dbus_deps = dependency('dbus-1', disabler: true, required: get_option('dbus'))
xsltproc = find_program('xsltproc', native: true, disabler: true, required: get_option('docs'))
check_deps = dependency('check', disabler: true, required: get_option('tests'))
tic = find_program('tic', required: false)

#
# Handle feature options
# This combines feature option inputs with dependency requirements to derive the
# final set of values that will determine what gets built.
# This will also store the information in a configuration header.
#
sections = {
  'video': 'Video Backends',
  'font': 'Font Backends',
  'renderer': 'Renderers',
  'session': 'Session Types',
}
config = configuration_data()
# Note: keep this in sync with the dependencies above
foreach name, reqs : {
  'libseat': [libseat_deps],
  'video_fbdev': [],
  'video_drm2d': [libdrm_deps],
  'video_drm3d': [libdrm_deps, gbm_deps, egl_deps, glesv2_deps],
  'renderer_gltex': [glesv2_deps],
  'font_unifont': [],
  'font_freetype': [freetype_deps, fontconfig_deps],
  'font_pango': [pango_deps],
  'session_dummy': [],
  'session_terminal': [],
  'dbus': [dbus_deps],
}
  found = true
  foreach req : reqs
    found = found and req.found()
  endforeach
  enabled = get_option(name).require(found).allowed()
  upper_name = name.to_upper()
  # set a variable for later use
  set_variable(f'enable_@name@', enabled)
  # set in config header for code use
  config.set(f'BUILD_ENABLE_@upper_name@', enabled)
  # summary output
  summary(name, enabled, section: sections.get(name.split('_')[0], 'Miscellaneous'))
endforeach

config.set('BUILD_ENABLE_DEBUG', get_option('extra_debug'))
config.set_quoted('BUILD_MODULE_DIR', prefix / moduledir)
config.set_quoted('BUILD_CONFIG_DIR', prefix / sysconfdir)

# Make all files include "config.h" by default. This shouldn't cause any
# problems and we cannot forget to include it anymore.
config_h = configure_file(configuration: config, output: 'config.h')
abs_config_h = meson.current_build_dir() / '@0@'.format('config.h')
add_project_arguments('-include', abs_config_h, language: 'c')

#
# Miscellaneous checks
# Not directly related to dependencies but
#
manpages_stylesheet = 'http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl'
have_manpages_stylesheet = false
if xsltproc.found()
  have_manpages_stylesheet = run_command(xsltproc, '--nonet', manpages_stylesheet, check: false).returncode() == 0
  if not have_manpages_stylesheet
    docs_error_string = 'docbook.xsl stylesheet was not found.'
  else
    docs_error_string = ''
  endif
else
  docs_error_string = 'executable xsltproc was not found.'
endif
enable_docs = get_option('docs').require(xsltproc.found() and have_manpages_stylesheet, 
  error_message: docs_error_string).allowed()

# Additionally check if glesv2 is needed
enable_glesv2 = enable_video_drm3d or enable_renderer_gltex

#
# Other configuration output
# Show configuration to the user so they can check whether everything was
# configured as expected.
#
summary({
  'prefix': prefix,
  'libexecdir': libexecdir,
  'bindir': bindir,
  'sysconfdir': sysconfdir,
  'moduledir': moduledir,
  'mandir': mandir,
  'systemdsystemunitdir': systemdsystemunitdir,
}, section: 'Directories')
summary({
  'extra_debug': get_option('extra_debug'),
  'tests': get_option('tests'),
  'docs': enable_docs,
}, section: 'Miscellaneous')

#
# Putting code together
#
subdir('src')

if get_option('tests')
  subdir('tests')
endif
if enable_docs
  subdir('docs')
endif

#
# Process other files
#
dirs_info = configuration_data()
dirs_info.set('libexecdir', prefix / libexecdir)
dirs_info.set('bindir', prefix / bindir)
if enable_libseat
  dirs_info.set('PAM_CFG', 'User=root\nPAMName=kmscon')
else
  dirs_info.set('PAM_CFG', '')
endif
foreach filename, kwargs : {
  'scripts/kmscon-launch-gui.sh': {
    'install_dir': bindir,
    'install_mode': 'rwxr-xr-x',
  },
  'scripts/systemd/kmscon.service.in': {
    'install_dir': systemdsystemunitdir,
    'install_mode': 'rw-r--r--',
  },
  'scripts/systemd/kmsconvt@.service.in': {
    'install_dir': systemdsystemunitdir,
    'install_mode': 'rw-r--r--',
  },
}
  install_data(configure_file(input: filename, output: '@BASENAME@', configuration: dirs_info),
    kwargs: kwargs,
  )
endforeach

# kmscon.conf
install_data(
  'scripts/etc/kmscon.conf.example',
  install_dir: join_paths(get_option('sysconfdir'), 'kmscon')
)
if enable_libseat
  install_data(
    'scripts/pam/kmscon',
    install_dir: get_option('sysconfdir') / 'pam.d',
    rename: 'kmscon',
  )
endif

# Terminfo
terminfo_src = files('scripts/terminfo/kmscon.ti')

if tic.found()
    # Schedule the script to run during 'meson install'
    meson.add_install_script(
        'scripts/terminfo/build_terminfo.py',
        prefix / datadir,
        terminfo_src
    )
else
    warning('The "tic" compiler was not found. Terminfo will not be installed.')
endif
