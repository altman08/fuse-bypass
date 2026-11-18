# 性能验证方案

目标：验证相同文件系统实现、相同工作负载和完成语义下，bypass 相比 FUSE 的延迟、吞吐和 CPU 收益。先测请求路径开销，再测实际存储场景。当前没有性能结论。

## 1. 对照组

| 组 | 应用路径 | 用途 |
| --- | --- | --- |
| Direct | io-bench → 示例 backing file | 估计本地存储基线 |
| FUSE | 同一 io-bench → 挂载点 → 目标回调 → 后端 | 对照组 |
| Bypass | 同一 io-bench + fuse-bypass.so → 同一目标回调 → 后端 | 实验组 |

三组串行运行，loopback 示例均使用同一个 backing/bench 文件。Direct 只适用于这个示例；真实目标没有等价的直接后端入口时，只比较 FUSE 与 Bypass，不拿其他文件系统作“同后端基线”。

必须保持一致：源码/编译参数、数据内容、文件大小、请求大小、操作次数、随机序列、CPU 亲和性、同步方式和后端配置。记录系统、内核、libfuse、CPU/NUMA、存储型号、挂载参数、CPU governor 和可用内存。

## 2. 测试阶段

### A. 验证路径和正确性

先运行 `scripts/test-mounted.sh`。该脚本包含小规模性能冒烟，但固定顺序和短运行不能当成最终结论。

另外单独运行带 `FUSE_BYPASS_STATS=1` 或 strace 的诊断：

- FUSE 读写进入守护进程回调。
- Bypass 读写进入应用内回调，不打开目标挂载路径的内核 fd。
- 非目标路径保留 libc 路径。
- 两条路径交替写入/读取，结果一致。

诊断与正式计时分开。不要把启用 strace 的数据纳入性能报告。回调计数包括文件准备阶段，不能直接与测量操作数一一对应。

### B. 测请求路径开销

使用同一台机器的 tmpfs（例如有足够容量的 `/dev/shm`）作为示例后端，工作集 16 MiB。三组均访问相同内存后端，只跑读和非持久化写；tmpfs 的 fsync 结果不能解释为持久化存储性能。

固定 FUSE `direct_io=1`，示例源码已经设置；后端仍使用普通 buffered I/O。FUSE direct_io 绕开的是 FUSE 文件的页缓存，不是后端 O_DIRECT。这一轮减少后端等待，用于观察请求往返开销。

| 负载 | 请求大小 | 初始操作次数 | 16 MiB 文件块数 |
| --- | --- | --- | --- |
| randread / randwrite | 4 KiB | 1,000,000 | 4096 |
| randread / randwrite | 16 KiB | 1,000,000 | 1024 |
| seqread / seqwrite | 128 KiB | 100,000 | 128 |
| seqread / seqwrite | 1 MiB | 10,000 | 16 |

目前所有负载使用 pread/pwrite；“顺序”表示偏移顺序递增，到文件尾后循环。结果不涵盖 open/close、目录操作和短命进程启动成本。

先试跑一次，根据耗时调整操作次数，争取 FUSE 测量阶段约 10–30 秒；同一负载三组使用相同次数。当前上限为 1000 万次，过短的样本增加重复轮数，报告实际耗时。不要只跑原冒烟脚本的 1 万次便得出收益结论。

### C. 测实际存储收益

把 backing 换成实际 SSD/HDD 所在目录，或编入实际用户文件系统并配置相同存储服务。重复阶段 B，再增加：

| 负载 | 请求大小 | 初始操作次数 | 完成条件 |
| --- | --- | --- | --- |
| syncwrite | 4 KiB | 10,000 | 每次 pwrite 后完成 fsync |
| syncwrite | 16 KiB | 10,000 | 每次 pwrite 后完成 fsync |

当前 syncwrite 使用顺序循环偏移，计时包含每次 fsync。seqwrite/randwrite 不包含持久化确认，只报告 buffered write 完成速度，不能把其结果称为磁盘持久化吞吐。每轮结束在测量外刷清后端脏数据，等后台 I/O 恢复空闲后再开始下一轮。

当前 io-bench 在计时前创建/截断文件、逐块写入并 fsync，所以这是预填充后的热缓存实验；不能通过在启动前 drop_caches 将它变成冷缓存实验。文件工作集须明显小于可用内存，并记录其大小。

## 3. 手动执行示例

以下命令用于默认 loopback 产物。选择两个允许使用的 CPU，尽量位于同一 NUMA 节点、不同物理核，避开 SMT 同核。应用三组固定在同一核，FUSE daemon 固定在另一核；不要将整个测量环境限制为只允许一个 CPU 后宣称双核配置的收益。

```sh
# 在项目根目录执行；修改 APP_CPU/FS_CPU 为实际可用 CPU。
export APP_CPU=2 FS_CPU=3
export DEMO_BACKING="/dev/shm/fb-perf-$$"
export FUSE_BYPASS_MOUNT="/tmp/fb-perf-mount-$$"
mkdir -p "$DEMO_BACKING" "$FUSE_BYPASS_MOUNT" results
unset LD_PRELOAD FUSE_BYPASS_STATS

taskset -c "$FS_CPU" ./build/target-fuse -f -s "$FUSE_BYPASS_MOUNT" >results/daemon.log 2>&1 &
FUSE_PID=$!
# 确认挂载完成后执行后续命令。
mountpoint "$FUSE_BYPASS_MOUNT"

# 一组 4 KiB 随机读，正式报告应按下节重复并轮换顺序。
taskset -c "$APP_CPU" ./build/io-bench \
  "$DEMO_BACKING/bench" randread 4096 1000000 4096 >results/direct-randread-4k.csv

taskset -c "$APP_CPU" ./build/io-bench \
  "$FUSE_BYPASS_MOUNT/bench" randread 4096 1000000 4096 >results/fuse-randread-4k.csv

taskset -c "$APP_CPU" env LD_PRELOAD="$PWD/build/fuse-bypass.so" \
  ./build/io-bench "$FUSE_BYPASS_MOUNT/bench" \
  randread 4096 1000000 4096 >results/bypass-randread-4k.csv

# 全部测试完成后卸载。
fusermount3 -u "$FUSE_BYPASS_MOUNT"
wait "$FUSE_PID"
```

先确认 tmpfs 容量足够。要测试实际磁盘，换用专门创建的测试目录，不要使用已有业务文件：io-bench 会截断传入文件。当前打包二进制要求 x86_64、glibc >= 2.28。

## 4. 重复与统计

每个负载先预热一组三路径并丢弃结果，再采集 9 轮。按以下顺序轮换三遍，每次另存文件，不覆盖上一轮：

1. Direct → FUSE → Bypass
2. FUSE → Bypass → Direct
3. Bypass → Direct → FUSE

三组不并发；排除其他压测和明显系统噪声。出现异常保留原始记录并调查原因，不只删除对结论不利的样本。

每轮记录现有 CSV 的 MiB/s、IOPS、P50/P99、应用 CPU 秒数与测量秒数。按轮计算 Bypass/FUSE 比值，报告 9 轮的中位数和范围；分别汇总每轮 P99，不将“各轮 P99 的平均值”解释为合并请求的 P99。大块看 MiB/s，小块看 IOPS/延迟。

收益计算：

- 吞吐提升：`(IOPS_bypass / IOPS_fuse - 1) × 100%`。
- P99 降低：`(P99_fuse - P99_bypass) / P99_fuse × 100%`。
- 对固定请求数，平均单请求耗时：`测量秒数 / 操作次数`。
- 相对 Direct 的可消除开销比例：`(T_fuse - T_bypass) / (T_fuse - T_direct)`，T 使用上述平均单请求耗时；仅在分母稳定为正且明显高于噪声时计算，保留负值或大于 100% 的原始结果并检查原因，不强行截断。

示例结果表：

| 后端/缓存 | 负载/块大小 | FUSE IOPS | Bypass IOPS | 吞吐提升 | FUSE P99 | Bypass P99 | 9 轮波动 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| tmpfs/热 | randread/4 KiB | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |
| SSD/热 | syncwrite/4 KiB | 待测 | 待测 | 待测 | 待测 | 待测 | 待测 |

若只在 tmpfs 有收益，实际存储没有，应报告“减少请求路径开销，但该实际负载受后端限制”。若仅 direct_io 配置有收益，结论必须带该条件，不能外推到生产缓存配置。

## 5. CPU、缓存与并发：第二阶段补齐

现有工具能先比较热缓存延迟/吞吐。以下项目需要补充测量能力后才能形成对应结论，不用当前数据代替：

- **总 CPU**：FUSE 为同一计时窗口内应用+守护进程的 user/system CPU；Bypass 为应用（含其后端线程）CPU。统一除以完成请求数，报告 CPU 秒/百万次 I/O；不要直接比两组瞬时 CPU%。当前脚本 daemon 的统计包含准备阶段，不能与 io-bench 的 app_cpu_seconds 直接相加。正式测量需在准备完成、测量开始/结束处设置同步标记，并在同一窗口采样 daemon 线程组 CPU；跨进程的额外后端服务也需计入或明确排除。
- **生产缓存配置**：另开一组启用目标原有 FUSE 缓存的测试，与对应 bypass 比较。当前示例把 direct_io 固定为 1，不能只改命令行就假定它已关闭，需要提供显式配置能力；不要同时通过两条路径修改可能缓存过期的数据。FUSE 缓存命中可能不进入 daemon，应允许这种配置下 FUSE 更快。
- **冷缓存/真实设备读**：先拆分 prepare/run，使缓存控制发生在准备之后、计时之前；冷热分别报告，不能与热缓存混合。缓存清理只针对隔离测试环境；当前版本不支持此流程。
- **并发扩展**：单进程多线程与多个独立进程分别测试 1/2/4/8 并发。当前拦截层持有全局 fd 锁串行执行回调，不能据单线程数据预测多线程扩展，也不要用多进程结果冒充同一进程多线程结果。
- **长时间写入稳定性**：buffered write 长时间运行可能触发后台回写；另测稳定态，并记录设备 I/O 与脏页情况，避免只报告短时缓存吸收速度。

第一轮验收不设“必须更快”的前提。先看小块热缓存下是否有可重复收益、P99 是否恶化，再看真实后端是否达到事先约定的业务目标。可以预先约定例如吞吐提升至少 10%、P99 不恶化作为工程门槛；这不是统计显著性的替代，收益还需明显大于轮间波动。

参考：[Linux FUSE I/O Modes](https://www.kernel.org/doc/html/latest/filesystems/fuse/fuse-io.html)。本方案关于当前基准计时、回调模式和统计边界的描述以仓库 tests/io-bench.c、examples/loopback.c、scripts/test-mounted.sh 为准。
