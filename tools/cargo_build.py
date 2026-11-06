#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a Rust static library with cargo, for meson.

Usage: cargo_build.py CARGO MANIFEST TARGET_DIR PROFILE OUTPUT DEPFILE

Runs cargo, copies the produced lib<name>.a to OUTPUT and writes a ninja
depfile listing every Rust source cargo used, so meson rebuilds on change.

Cargo runs in the crate directory, so that its .cargo/config.toml (which
selects the vendored crates) is used, and with --frozen, so that it never
accesses the network nor changes Cargo.lock.
"""

import os
import shutil
import subprocess
import sys


def main():
    cargo, manifest, target_dir, profile, output, depfile = sys.argv[1:7]

    cmd = [
        cargo,
        'build',
        '--manifest-path',
        manifest,
        '--target-dir',
        target_dir,
        '--profile',
        profile,
        '--frozen',
    ]
    ret = subprocess.run(cmd, cwd=os.path.dirname(os.path.abspath(manifest))).returncode
    if ret:
        return ret

    # the "dev" profile builds into target/debug
    profile_dir = 'debug' if profile == 'dev' else profile
    name = os.path.basename(output)
    built = os.path.join(target_dir, profile_dir, name)
    shutil.copyfile(built, output + '.tmp')
    os.replace(output + '.tmp', output)

    # cargo writes "target/<profile>/libfoo.a: deps...", ninja wants OUTPUT
    cargo_dep = os.path.splitext(built)[0] + '.d'
    with open(cargo_dep) as f:
        content = f.read()
    _, sep, deps = content.partition(': ')
    if not sep:
        deps = ''
    with open(depfile, 'w') as f:
        f.write(f'{output}: {deps}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
