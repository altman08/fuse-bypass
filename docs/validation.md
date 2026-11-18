# Validation record

Build: Linux x86_64, GCC 4.8.5, private libfuse 3.10.5, FUSE API 31.
Packaged libfuse requires GLIBC_2.28; all binaries use dynamic linking.

Completed:

- Build with -Wall -Wextra -Werror (project code).
- make check: ordinary and 64-bit/fortified POSIX entry points; create,
  truncate, exclusive create, access-mode errors, offsets, independent opens,
  positioned I/O, EOF/short reads, real fsync/fdatasync, repeated fd reuse,
  use after close, missing paths, mount-prefix boundary, symlink fallback,
  unsupported append fallback, disabled interception, and target argv.
- Compile upstream libfuse 3.10.5 example/hello.c without source modifications;
  read via its callbacks and verify missing fsync/fdatasync returns EOPNOTSUPP.
  Upstream's existing signedness warning is allowed only for its source.
- Smoke-run all five benchmark workloads through direct and bypass paths.
  These smoke tests are not evidence of a FUSE speedup.
- strace -f confirms bypass write does not open the target mount path in the
  kernel, and does invoke backing pwrite64 via the example target callback.
- Extract the packaged archive to another directory, rerun correctness checks,
  and launch target-fuse --version using the bundled shared library.
- Shell syntax and git whitespace checks.

Not run here:

- Actual FUSE mounts: this environment has no /dev/fuse.
- Mounted FUSE <-> bypass coherence and three-path performance comparison:
  scripts/test-mounted.sh supplies these checks for the test host.
- User's actual filesystem: its source has not been supplied. Standard
  high-level startup was verified with the two examples above; the documented
  restrictions still apply to other filesystems.
- Cold-cache, concurrent mixed access, multi-thread scaling, and precisely
  aligned app+daemon CPU accounting.
