#!/bin/sh
# Private build of a pinned libfuse3; does not install system-wide.
set -eu
cd "$(dirname "$0")/.."
export LC_ALL=C
mkdir -p .deps
if [ ! -f .deps/libfuse-3.10.5.tar.gz ]; then
    curl -fL --retry 2 --max-time 120 https://codeload.github.com/libfuse/libfuse/tar.gz/refs/tags/fuse-3.10.5 -o .deps/libfuse-3.10.5.tar.gz
fi
sha256sum -c scripts/libfuse.sha256
tar -xzf .deps/libfuse-3.10.5.tar.gz -C .deps
if [ ! -f .deps/tools/mesonbuild/mesonmain.py ]; then
    python3 -m pip install --target .deps/tools 'meson==0.59.4' 'ninja==1.10.2.4'
fi
export PYTHONPATH="$PWD/.deps/tools"
export NINJA="$PWD/.deps/tools/ninja/data/bin/ninja"
if [ ! -f .deps/fuse-build/build.ninja ]; then
    python3 -m mesonbuild.mesonmain setup .deps/fuse-build .deps/libfuse-fuse-3.10.5 \
        --prefix="$PWD/.deps/fuse" --libdir=lib -Dexamples=false -Dtests=false \
        -Dutils=false -Duseroot=false -Ddisable-mtab=true
fi
"$NINJA" -C .deps/fuse-build install
