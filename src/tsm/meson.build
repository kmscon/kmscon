# SPDX-License-Identifier: MIT
#
# libtsm (Rust) and its C bindings (src/ffi.rs), built by cargo as a static
# library.
#
# libtsm_deps: headers + static library, for executables. The kmscon binary
#              links the whole archive and exports the symbols (export_dynamic)
#              so that plugins can use them.
# libtsm_headers_deps: headers only, for shared modules and static libraries
#              that end up in them; they resolve tsm_* from the kmscon binary.

cargo = find_program('cargo')

# debug builds use the cargo dev profile, everything else release
if get_option('buildtype') in ['debug', 'plain']
    cargo_profile = 'dev'
else
    cargo_profile = 'release'
endif

tsm_manifest = meson.current_source_dir() / 'Cargo.toml'
tsm_target_dir = meson.current_build_dir() / 'cargo-target'

tsm_lib = custom_target(
    'libtsm',
    output: 'libtsm.a',
    depfile: 'libtsm.d',
    depend_files: files('Cargo.toml', 'Cargo.lock'),
    console: true,
    command: [
        python,
        meson.project_source_root() / 'tools' / 'cargo_build.py',
        cargo,
        tsm_manifest,
        tsm_target_dir,
        cargo_profile,
        '@OUTPUT@',
        '@DEPFILE@',
    ],
)

# from: cargo rustc -- --print native-static-libs
tsm_native_libs = [
    '-lgcc_s',
    '-lutil',
    '-lrt',
    '-lpthread',
    '-lm',
    '-ldl',
    '-lc',
]

tsm_inc = include_directories('.')

libtsm_headers_deps = declare_dependency(include_directories: tsm_inc)

libtsm_deps = declare_dependency(
    include_directories: tsm_inc,
    link_with: tsm_lib,
    link_args: tsm_native_libs,
)
