# libtsm tests, like cargo test.
# The golden and stress tests are cargo integration tests: separate crates
# using the Rust API, so they need the library as an rlib, named so that its
# crate name is "tsm". (rust.test() would only build the #[cfg(test)] unit
# tests of the library, and there are none.)
tsm_rlib = tsm_pkg.library('tsm+5', rust_abi: 'rust')
rust.doctest('tsm_doc', tsm_rlib)

foreach name, src : {
    'golden': 'golden/main.rs',
    'stress': 'stress.rs',
}
    tsm_test = executable(
        f'tsm_@name@',
        src,
        rust_args: ['--test'],
        link_with: tsm_rlib,
        override_options: ['rust_std=2024'],
    )
    test(
        f'tsm_@name@',
        tsm_test,
        args: ['--format', 'pretty'],
        protocol: 'rust',
        # the golden test looks for its scenarios relative to it
        env: {'CARGO_MANIFEST_DIR': meson.project_source_root()},
    )
endforeach
