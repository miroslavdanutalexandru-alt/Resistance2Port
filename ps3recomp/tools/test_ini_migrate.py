#!/usr/bin/env python3
"""port_ini_migrate.h: renamed settings.ini keys (project/tests/ini_migrate_test.cpp)."""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def main():
    src = REPO / 'project' / 'tests' / 'ini_migrate_test.cpp'
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++')
    if not cxx:
        print('SKIP: no C++ compiler (set CXX)')
        return
    with tempfile.TemporaryDirectory(prefix='ini-migrate-') as tmp:
        exe = Path(tmp) / ('ini_migrate_test' + ('.exe' if os.name == 'nt' else ''))
        subprocess.run([cxx, '-std=c++17', '-O1', '-I' + str(REPO / 'project'), str(src), '-o', str(exe)],
                       check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=20)


if __name__ == '__main__':
    main()
