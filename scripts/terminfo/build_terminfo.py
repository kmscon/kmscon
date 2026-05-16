#!/usr/bin/env python3
import os
import subprocess
import sys

# Meson passes DESTDIR automatically via the environment
destdir = os.environ.get('DESTDIR', '')
datadir = sys.argv[1]
terminfo_src = sys.argv[2]

# Standard destination is /usr/share/terminfo (or /usr/local/share/terminfo)
# lstrip prevents os.path.join from treating datadir as an absolute path anchors
target_dir = os.path.join(destdir, datadir.lstrip('/'), 'terminfo')

# Ensure the base terminfo directory exists
os.makedirs(target_dir, exist_ok=True)

# Run the terminfo compiler
# -x forces tic to handle non-standard/extended capabilities if present
# -o defines the output directory root
print(f'Compiling terminfo to {target_dir}...')
subprocess.run(['tic', '-x', '-o', target_dir, terminfo_src], check=True)