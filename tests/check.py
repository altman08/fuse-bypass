import os
from pathlib import Path
import subprocess
import sys
import tempfile

build = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix='fuse-bypass-') as tmp:
    root = Path(tmp)
    mount, backing, other = (root / n for n in ('mount', 'backing', 'mount-other'))
    for p in (mount, backing, other):
        p.mkdir()
    env = dict(os.environ, LD_PRELOAD=str(build / 'fuse-bypass.so'),
               FUSE_BYPASS_MOUNT=str(mount), DEMO_BACKING=str(backing), FUSE_BYPASS_STATS='1')
    def run(cmd, path, *args, **kw):
        exe = kw.get('exe', 'io-test')
        e = kw.get('env', env)
        r = subprocess.run([str(build / exe), cmd, str(path)] + list(args), env=e,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=30)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)
        return r
    for exe in ('io-test', 'io-test64'):
        r = run('verify', mount / 'data', exe=exe)
        assert 'target_open=0 ' not in r.stderr, r.stderr
        assert (backing / 'data').exists() and not (mount / 'data').exists()
        run('verify', other / 'plain', exe=exe)
        assert not (backing / 'plain').exists()
    run('write', mount / 'shared', 'through callbacks')
    run('expect', backing / 'shared', 'through callbacks', env=os.environ.copy())
    run('write', backing / 'shared', 'from backend', env=os.environ.copy())
    run('expect', mount / 'shared', 'from backend')
    assert run('route', mount / 'shared').stdout.strip() == 'bypass'
    run('write', other / 'shared', 'outside')
    assert run('route', other / 'shared').stdout.strip() == 'kernel'
    (mount / 'shared').write_text('kernel')
    run('append', mount / 'shared')
    assert (mount / 'shared').read_text() == 'kernel!'
    assert (backing / 'shared').read_text() == 'from backend'
    # Symlink detected by target getattr: use the mounted view wholesale.
    (backing / 'link').symlink_to('shared')
    (mount / 'link').write_text('kernel-only')
    assert run('route', mount / 'link').stdout.strip() == 'kernel'
    assert run('route', str(mount) + '/../mount/link').stdout.strip() == 'kernel'
    # Registration absent: preload is a no-op and target main is never started.
    disabled = dict(env)
    disabled.pop('FUSE_BYPASS_MOUNT')
    disabled.pop('DEMO_BACKING')
    run('verify', other / 'disabled', env=disabled)
    # The real target main receives argv from a file without source changes.
    args = root / 'args'
    args.write_text('-f\n' + str(mount) + '\n')
    configured = dict(env, FUSE_BYPASS_ARGS_FILE=str(args))
    run('expect', mount / 'shared', 'from backend', env=configured)
print('PASS: callbacks, libc fallback, fd lifetime, offsets, errors, aliases, shared data, target argv')
