# SPDX-License-Identifier: MIT
#
# libtsm (Rust) and its C bindings (ffi.rs), built by cargo as a static
# library.
#
# libtsm_deps: headers + static library, for executables. The kmscon binary
#              links the whole archive and exports the symbols (export_dynamic)
#              so that plugins can use them.
# libtsm_headers_deps: headers only, for shared modules and static libraries
#              that end up in them; they resolve tsm_* from the kmscon binary.

tsm_pkg = rust.workspace().package('libtsm')
tsm_lib = tsm_pkg.library('tsm', rust_abi: 'c')

tsm_inc = include_directories('.')

libtsm_headers_deps = declare_dependency(include_directories: tsm_inc)

libtsm_deps = declare_dependency(
    include_directories: tsm_inc,
    link_with: tsm_lib,
)
