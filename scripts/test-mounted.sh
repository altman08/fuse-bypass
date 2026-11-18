#!/bin/sh
# Run on the test host with libfuse3 tools, /dev/fuse and mount permission.
set -eu
cd "$(dirname "$0")/.."
project=$PWD
work=$(mktemp -d /tmp/fuse-bypass-mounted.XXXXXX)
mkdir "$work/mount" "$work/backing"
export DEMO_BACKING="$work/backing"
pid=
cleanup() {
    if mountpoint -q "$work/mount"; then fusermount3 -u "$work/mount" || true; fi
    if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    echo "Logs and test data: $work"
}
trap cleanup EXIT INT TERM
FUSE_BYPASS_STATS=1 ./build/target-fuse -f -s "$work/mount" >"$work/daemon.log" 2>&1 &
pid=$!
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    mountpoint -q "$work/mount" && break
    kill -0 "$pid" 2>/dev/null || { cat "$work/daemon.log"; exit 1; }
    sleep 1
done
mountpoint -q "$work/mount"
./build/io-test verify "$work/mount/check"
./build/io-test write "$work/mount/shared" fuse-to-bypass
export FUSE_BYPASS_MOUNT="$work/mount"
LD_PRELOAD="$project/build/fuse-bypass.so" ./build/io-test expect "$work/mount/shared" fuse-to-bypass
LD_PRELOAD="$project/build/fuse-bypass.so" ./build/io-test write "$work/mount/shared" bypass-to-fuse
./build/io-test expect "$work/mount/shared" bypass-to-fuse
LD_PRELOAD="$project/build/fuse-bypass.so" ./build/io-test64 verify "$work/mount/check64"
uname -a >"$work/environment.txt"
ldd --version >>"$work/environment.txt" 2>&1
fusermount3 --version >>"$work/environment.txt" 2>&1
# Collect daemon CPU ticks around the FUSE benchmark batch, separately from app CPU.
# Counters include all requests within the batch (including benchmark preparation).
cp "/proc/$pid/stat" "$work/daemon-cpu-before.txt"
for mode in seqread seqwrite randread randwrite syncwrite; do
    ./build/io-bench "$work/mount/bench" "$mode" 4096 10000 1024 >"$work/fuse-$mode.csv"
done
cp "/proc/$pid/stat" "$work/daemon-cpu-after.txt"
getconf CLK_TCK >"$work/clock-ticks-per-second.txt"
for mode in seqread seqwrite randread randwrite syncwrite; do
    ./build/io-bench "$work/backing/bench" "$mode" 4096 10000 1024 >"$work/direct-$mode.csv"
    LD_PRELOAD="$project/build/fuse-bypass.so" ./build/io-bench "$work/mount/bench" "$mode" 4096 10000 1024 >"$work/bypass-$mode.csv"
done
echo 'PASS: mounted FUSE <-> bypass consistency, correctness and benchmark runs'
