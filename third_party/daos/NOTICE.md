# DAOS code attribution

Upstream: https://github.com/daos-stack/daos
Commit: dcb0e18cf66264857a78b31e1e96c4d0648b08af
License: BSD-2-Clause-Patent, reproduced in LICENSE.

src/intercept.c contains code derived from:

- src/client/dfuse/pil4dfs/int_dfs.c
  Copyright 2022-2024 Intel Corporation;
  Copyright 2025-2026 Hewlett Packard Enterprise Development LP.
- src/client/dfuse/pil4dfs/pil4dfs_int.h
  Copyright 2024 Intel Corporation.

query_dfs_mount is copied with formatting changes only. read_comm and
write_comm are copied with the bash/compatible-mode special case removed.
FD_FILE_BASE and MAX_OPENED_FILE are retained. pread/pwrite dispatch and the
lseek switch are adapted. open_common retains the original routing structure,
with DAOS object management replaced by FUSE callback management.

Local adaptations: no DAOS runtime dependencies, one configured mount,
restricted path syntax, callback-based handles, bounds checks, serialized fd
lifetime, real fsync callback, no directory/dup/mmap/compatible-mode machinery.
The target-main bridge and build/test code are local additions, not DAOS code.
See docs/daos-reference.md for the source mapping and semantic differences.
