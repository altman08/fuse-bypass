"""Exercise an unmodified upstream FUSE filesystem linked into the library."""
import os
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='fb-upstream-') as mount:
    env = dict(os.environ, LD_PRELOAD=str(project / 'build-upstream/fuse-bypass.so'),
               FUSE_BYPASS_MOUNT=mount)
    for cmd in (['expect', mount + '/hello', 'Hello World!\n'], ['syncfail', mount + '/hello']):
        subprocess.run([str(project / 'build/io-test')] + cmd, env=env, check=True, timeout=30)
print('PASS: unmodified upstream hello.c, init/context, missing fsync returns EOPNOTSUPP')
