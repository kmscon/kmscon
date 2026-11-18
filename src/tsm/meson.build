# SPDX-License-Identifier: MIT
#
# libtsm (Rust) and its C bindings (ffi.rs) are part of the kmscon-rs crate
# (see src/meson.build).
#
# libtsm_headers_deps: headers only, for shared modules and static libraries
#              that end up in them; they resolve tsm_* from the kmscon binary.

tsm_inc = include_directories('.')

libtsm_headers_deps = declare_dependency(include_directories: tsm_inc)
