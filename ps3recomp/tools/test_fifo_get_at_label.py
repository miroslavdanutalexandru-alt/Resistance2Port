#!/usr/bin/env python3
"""R3_FIFO_GET_AT_LABEL: GET is past a label release when its value lands (third_party/rpcs3/.../RSX/port_get_at_label.h)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
src = root / 'tests/test_fifo_get_at_label.c'
inc = root / 'third_party/rpcs3/rpcs3/Emu/RSX'
with tempfile.TemporaryDirectory(prefix='fifo-get-at-label-') as tmp:
    if os.name == 'nt':
        cl = shutil.which('clang-cl') or r'C:\Program Files\LLVM\bin\clang-cl.exe'
        if not Path(cl).exists() or not os.environ.get('LIB'):
            print('SKIP: needs clang-cl and an MSVC environment (vcvars64)')
            raise SystemExit(0)
        binary = Path(tmp) / 'test.exe'
        subprocess.run([cl, '/nologo', '/O1', '/W4', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(inc),
                        str(src), '/Fe:' + str(binary), '/Fo:' + str(Path(tmp) / 'test.obj')], check=True, cwd=tmp)
    else:
        binary = Path(tmp) / 'test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-Wall', '-I' + str(inc),
                        str(src), '-o', str(binary)], check=True)
    r = subprocess.run([str(binary)], timeout=60, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    print(r.stdout.strip())
    if r.returncode:
        print(r.stderr.strip())
    raise SystemExit(r.returncode)
