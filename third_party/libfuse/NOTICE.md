The private bootstrap build uses unmodified libfuse 3.10.5:
https://github.com/libfuse/libfuse/tree/fuse-3.10.5

The libfuse shared library is licensed under LGPL 2.1; see LICENSE and LGPL2.txt.
GPL2.txt is also included because the complete upstream source distribution
contains components under that license. The binary package includes the exact
source archive at .deps/libfuse-3.10.5.tar.gz. scripts/bootstrap-fuse.sh documents
how to rebuild it and scripts/libfuse.sha256 records the archive digest.
The shared library is dynamically linked and can be replaced/relinked.
