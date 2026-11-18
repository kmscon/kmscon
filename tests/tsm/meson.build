# libtsm tests, like cargo test.
# The golden and stress tests are cargo integration tests: separate crates
# using the Rust API, so they link the library as an rlib (kmscon_rs_rlib).
# (The #[cfg(test)] unit tests are run by the kmscon_rs test, see
# src/meson.build.)
rust.doctest('tsm_doc', kmscon_rs_rlib)

foreach name, src : {
    'golden': 'golden/main.rs',
    'stress': 'stress.rs',
}
    tsm_test = executable(
        f'tsm_@name@',
        src,
        rust_args: ['--test'],
        link_with: kmscon_rs_rlib,
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
