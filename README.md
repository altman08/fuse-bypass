# fuse-bypass

一个验证 **通过用户态函数拦截，绕过 FUSE 请求路径** 的实验项目。

同一套文件系统核心提供两个入口：普通应用通过 FUSE 挂载访问；加载拦截库的应用通过用户态接口直接访问。比较两条路径的正确性、延迟、吞吐和 CPU 开销，判断绕过 FUSE 是否值得。

demo 已实现并完成本地编译及无需挂载的正确性验证；真实 FUSE 挂载对照和性能结论待测试机器验证。分流代码直接移植自固定版本 DAOS，具体对应关系见 [源码分析](docs/daos-reference.md) 和 [移植声明](third_party/daos/NOTICE.md)。

## 快速使用

```sh
# 有系统 fuse3 开发包可直接 make；否则在项目目录私有构建依赖
./scripts/bootstrap-fuse.sh
make
make check
make check-upstream  # 可选，需要 bootstrap 下载的官方源码
make package
```

产物：`build/fuse-bypass.so`、`build/target-fuse`、`build/io-test`、`build/io-test64`、`build/io-bench`；私有构建时附带 `build/libfuse3.so.3`。打包结果是 `dist/fuse-bypass-x86_64.tar.gz`，包含运行依赖、测试脚本、项目源码、许可证和所附 libfuse 的对应源码归档。当前打包产物要求 Linux x86_64、glibc **2.28 或更新**（所附 libfuse 的符号要求）；更旧 glibc 的机器需在兼容环境重新编译。不是静态或跨架构二进制。

无需挂载也能验证回调路径：

```sh
mkdir -p /tmp/fb-demo/mount /tmp/fb-demo/backing
DEMO_BACKING=/tmp/fb-demo/backing \
FUSE_BYPASS_MOUNT=/tmp/fb-demo/mount \
FUSE_BYPASS_STATS=1 \
LD_PRELOAD="$PWD/build/fuse-bypass.so" \
  ./build/io-test verify /tmp/fb-demo/mount/test
```

在支持 FUSE 的测试机器解压后，从项目根目录运行：

```sh
./scripts/test-mounted.sh
```

脚本启动示例 FUSE 文件系统，验证两条路径双向数据一致性，运行三路径基准，退出时卸载并保留日志。需要 `/dev/fuse`、挂载权限、`fusermount3` 和 `mountpoint`。示例以 `direct_io`、元数据缓存超时为 0、单线程运行；后台存储页缓存仍然存在。基准只操作脚本创建的临时文件。

### 将用户文件系统一起编译

```sh
# 源码不需要包含本项目头文件，也不需要导出插件接口
make BUILD=build-user \
  TARGET_SOURCES='/absolute/myfs/main.c /absolute/myfs/storage.c' \
  TARGET_CPPFLAGS='-I/absolute/myfs/include' \
  TARGET_LDLIBS='-lmybackend'
```

`TARGET_SOURCES` 当前接收 C 源文件，需使用 libfuse3 高层 `fuse_main(argc, argv, &ops, private_data)` 启动。Makefile 只在编入共享库时通过 `-Dmain=fuse_bypass_target_main` 重命名入口，链接时 `--wrap=fuse_main_real` 捕获原回调表；另一个 `target-fuse` 使用原始入口编译。用户启动函数运行在专用线程，停留在注册入口直到退出，避免提前执行 `main` 中的清理逻辑。

通过 `FUSE_BYPASS_MOUNT` 指定与编入文件系统对应的规范绝对目录。默认启动参数为 `-f <目录>`；额外参数可用 `FUSE_BYPASS_ARGS_FILE=/absolute/args.txt` 传入，每行一个 argv 元素，不包括 argv[0]，不经过 shell 展开。例如文件内容：

```text
--backend=server-address
-f
/mnt/myfs
```

此时使用 `LD_PRELOAD="$PWD/build-user/fuse-bypass.so"`。每个共享库包含一份目标文件系统实现；不能仅指定一个任意现有挂载点就自动取得该守护进程的代码或内存状态。

### 当前兼容边界

- 接管 libc 动态符号，不覆盖直接 syscall、静态程序或 libc 内部隐藏调用。目标文件只使用本 README 列出的 fd 接口；`openat`、stdio、stat、dup、fcntl、mmap、文件锁、fork/exec 等不在支持范围。
- 路径匹配直接移植 DAOS 的目录边界判断。相对路径、`..`、多余斜线、目标符号链接、目录及不支持的打开标志（如 `O_APPEND/O_DIRECT/O_SYNC`）在打开时整体走 libc。启动时读取嵌套挂载清单，运行中不能改变挂载拓扑。尚未打开的目标文件若缺必要回调也整体走 libc。
- 对已接管 fd，读写失败不会改走内核；同步缺失返回 `EOPNOTSUPP`，关闭时总是释放句柄。用户态 fd 使用 DAOS 的 `0x20000000` 起始值，最多 2048 个；启动时检查 Linux `nr_open` 上界避免冲突，运行中不要修改该上界。
- `init/destroy` 和 `fuse_get_context()->private_data/uid/gid/pid/umask` 已桥接。没有真实 FUSE session，context 的 `fuse` 为 NULL；`init` 得到无 capabilities 的本地连接描述，要求真实内核特性的目标不适用。umask 在首次启动目标时读取，测试期间不要更改。
- `fuse_main` 内部的挂载选项解析、内核权限检查和缓存机制不会在 bypass 中重现；用户 `main` 自己的参数解析仍会执行。不要依赖 `-o ro/default_permissions/modules=...` 等选项来限定直接回调的行为。文件系统回调及其后端需要承担访问检查。
- 目标启动代码必须可以在应用进程中运行，不能在注册前调用 `exit`、daemonize、改变进程工作目录或阻塞等待 FUSE 请求；自定义低层 session/事件循环不支持。目标额外创建的线程不自动继承 hook 递归保护。
- 同一进程内回调用互斥锁串行化；不承诺线程扩展性能、跨进程状态共享或 FUSE 与 bypass 的并发缓存一致性。

已验证本项目示例以及**未经源码修改**的 libfuse 3.10.5 官方 `example/hello.c`（只读），但尚未获得用户实际文件系统源码，因此尚未验证其初始化和回调兼容性。

## 1. 核心想法

利用 `LD_PRELOAD` 加载 `fuse-bypass.so`，接管应用的 `open/read/write` 等 libc/POSIX 函数调用：

- 对于属于目标文件系统的路径和 fd，通过 `fuseapi` 直接调用目标 FUSE 文件系统；使用 libfuse3 的 `struct fuse_operations` 回调表。
- 对于其他文件，调用原来的 libc 函数，继续走内核文件系统路径。
- FUSE 前端与拦截库共用后端逻辑，避免实现两套不同的文件系统语义。

这里的“拦截”发生在用户态函数层，不是修改内核 syscall 表，也不是拦截所有直接执行的系统调用。其机制与通过预加载共享库替换 malloc 类似。

```text
普通路径：
应用 → libc → syscall → VFS/FUSE → FUSE 守护进程 → 文件系统核心 → 存储后端

实验路径：
应用 → fuse-bypass.so → libfuse3 fuse_operations 回调 → 目标文件系统 → 存储后端
              │
              └─ 非目标文件 → 原 libc → 内核 → 原文件系统
```

目标是减少 FUSE 请求封装、跨进程传递、线程唤醒、排队及可能的数据拷贝。**不承诺整个 I/O 链路没有系统调用**：如果后端使用本地 `pread/pwrite` 或 socket，后端仍然会进入内核。

## 2. 如何识别需要接管的文件

### 路径操作

通过配置指定目标路径，例如 `/mnt/fuse-bypass`。`open` 时判断文件是否属于这个命名空间：

```text
/mnt/fuse-bypass/a.db → 本项目后端
/home/user/a.txt     → 原 libc
/mnt/nfs/a.txt       → 原 libc
```

判断标准是“是否由本项目管理”，不是“是否为远程文件”。NFS 或其他 FUSE 文件系统也应保持原来的处理路径。

不能只做不带边界判断的字符串前缀匹配：`/mnt/fuse-bypass-other` 不属于目标目录。后续需要处理相对路径、`..`、`openat`、符号链接和嵌套挂载；第一版通过明确限制输入，缩小实现范围。

### fd 操作

`read/write` 只有 fd，没有路径，因此拦截库需要维护映射：

```text
应用 fd → 后端文件句柄、打开标志、当前偏移及生命周期状态
```

简化逻辑如下，实际实现还需处理并发、错误码和生命周期：

```c
ssize_t hooked_read(int fd, void *buf, size_t size)
{
    struct managed_file *file = lookup_managed_fd(fd);
    if (file != NULL)
        return backend_read(file, buf, size);
    return real_read(fd, buf, size);
}
```

原函数可通过 `dlsym(RTLD_NEXT, ...)` 获取。初始化和后端访问必须避免递归进入自己的 hook；目标文件系统若访问本地存储，应避免再次经由目标挂载路径访问自身。

## 3. 第一版 demo 的范围

先实现一个可控的 C 测试程序，而不是承诺兼容任意程序。

### 组件

| 组件 | 职责 |
| --- | --- |
| 目标文件系统 / `fuseapi` | 用户源码与拦截库一起编译，通过标准 fuse_main 注册回调 |
| 目标文件系统的 FUSE 前端 | 提供普通挂载访问路径，作为对照 |
| `fuse-bypass.so` | 识别目标路径、管理 fd、将调用转到 `fuseapi` 或原 libc |
| 测试程序 | 使用 POSIX 接口验证两条路径，并执行性能测试 |

已确定后端通过 `fuseapi` 直接调用目标 FUSE 文件系统，拦截层不直接实现普通目录映射。仓库的 `examples/loopback.c` 是用于验收的独立 FUSE 文件系统，其自身使用目录存储；接入真实目标时将该源码替换为用户源码。FUSE 模式和 bypass 模式应访问同一目标文件系统的数据。`fuseapi` 已确定为 libfuse3 的 `struct fuse_operations`。用户文件系统源码与本项目一起编译，保留其 `fuse_operations`、回调签名及标准 `fuse_main` 调用；通过编译参数重命名用户 `main`，并接管注册入口，不要求新增插件接口或修改用户源码。不能仅凭挂载点获得另一个进程的函数指针。

如果现有文件系统的核心只能运行在守护进程内，需要另外设计客户端通信；用 socket 转发仍有 IPC 和系统调用开销，应作为另一种方案单独测量。

### 构建与运行环境

本机编译生成 `fuse-bypass.so` 和测试二进制，复制到另一台机器运行验证。已在项目私有 `.deps/` 下构建 libfuse 3.10.5；本机缺少 `/dev/fuse`，不以本机成功挂载作为验收条件。按当前环境的 Linux x86_64（64 位 x86）编译，使用 libfuse3。目标机器需满足产物的 glibc 和动态库依赖；若目标是 32 位 x86，需另行交叉编译。

### 已实现的接口

- `open/open64/creat/creat64`、fortify open 入口及 `close`：建立和释放映射。
- `read/write`：维护当前文件偏移。
- `pread/pwrite` 及 64 位别名：带偏移访问，不改变当前偏移。
- `lseek`：支持测试程序需要的定位操作。
- `fsync/fdatasync`：转到后端实际同步操作，不能直接假装成功。

第一版只接管目标目录下的普通文件，使用绝对路径，不使用符号链接、嵌套挂载或 `..`。受控测试程序先不使用 `openat`、stdio、`mmap`、`dup`、文件锁、`fork/exec`、直接 syscall 或 `io_uring`。

fd 分流参考 DAOS `libpil4dfs` 普通模式：为目标文件分配用户态 fd，维护回调句柄、标志、偏移和生命周期；非目标 fd 调原 libc。启动时检查 fd 区间不会与内核分配的 fd 重叠。未覆盖的 fd API 不具备目标文件语义，不能将用户态 fd 当成内核文件使用。具体源码依据、拦截列表与裁剪差异见 [DAOS 源码分析](docs/daos-reference.md)。

### 后续扩展

- `openat`、相对路径及符号链接解析。
- `stat/fstat`、目录、创建删除和重命名。
- `dup/dup2/fcntl`：正确共享打开文件状态和偏移。
- 多线程、进程继承、文件锁、`mmap`。
- 适配自定义 FUSE 事件循环和依赖真实内核连接的目标文件系统。
- 为 LevelDB 实现自定义 `Env`，作为显式调用用户态接口的对照方案。

这些功能不属于第一版必须完成的范围。

## 4. 正确性与性能验证

可执行步骤、对照组、重复统计方法和当前工具限制见 [性能验证方案](docs/performance-plan.md)。

### 正确性优先

1. 非目标目录的文件读写仍走原来的系统路径。
2. 目标文件通过 FUSE 写入后，关闭并按受控缓存条件重新打开，bypass 能读出一致数据；反方向同样验证。
3. 验证偏移、EOF、短读写、创建与截断、错误码和关闭后资源释放。
4. 验证不同 fd 和多次打开不会错误共享偏移。
5. 用 hook 计数器、FUSE 回调计数器和 `strace` 辅助确认调用路径。

`strace` 只用于确认路径，正式性能测试时关闭。后端仍有 syscall 是正常的，判断依据是目标请求是否跳过了 FUSE 前端，不是进程的 syscall 数量必须为零。

### 对照实验

| 模式 | 路径 | 用途 |
| --- | --- | --- |
| 后端直接访问 | 示例中测试程序直接访问 loopback 存储目录 | 基线；真实后端需提供相应基线 |
| FUSE | 测试程序 → 挂载点 → FUSE → 同一后端 | 测量 FUSE 路径 |
| Bypass | 同一测试程序 + 预加载库 → 同一后端 | 测量绕过后的路径 |

已提供单线程小块随机读写、顺序读写、同步写；块大小可配置为大块。输出吞吐、IOPS、P50/P99 延迟和应用进程 CPU 时间。异机脚本另外保存守护进程 CPU ticks（包含准备阶段，不能直接与应用计时区间相加）。多线程、精确对齐的合计 CPU 和冷缓存实验留作后续。

记录内核和 libfuse 版本、挂载参数、请求大小、线程数、缓存状态及同步策略。热缓存和冷缓存分别测量；`write()` 返回不等于数据持久化，不能拿不同持久化语义的结果直接比较。

FUSE 页缓存命中的读请求可以不进入守护进程，writeback 也会改变请求粒度和完成时机。可以先测明确配置的 FUSE direct-I/O 模式以观察请求往返，再测实际使用的缓存模式。这里的 FUSE `direct_io` 是绕过 FUSE 文件的页缓存，不是绕过 FUSE，也不自动等价于后端 `O_DIRECT`。

第一版不承诺 FUSE 与 bypass 并发访问时的完整缓存一致性。混合访问需要设计失效通知或限制缓存；仅在两条路径之间共用后端数据，不会自动解决 FUSE 缓存过期问题。

## 5. DAOS 参考

DAOS 提供了可以参考的结构：`dfuse` 和拦截库最终都调用 `libdfs`，后者提供 DAOS 上的文件和目录抽象。

- **libioil**：主要绕过数据读写路径。接管打开、关闭等操作以维护状态，并可通过 dfuse 专用 ioctl 获取 DAOS 对象和句柄信息；接管函数不代表该操作完全绕过内核。
- **libpil4dfs**：覆盖更多数据和元数据接口，通过路径识别及 fd 管理，将目标操作转换为 DFS 调用。除符号替换外，源码还包含共享库函数入口 hook 机制。
- DAOS 的库依赖 DFS/DAOS 后端及 dfuse 协议，不能直接作为任意 FUSE 文件系统的通用加速库。本项目参考其分流和共用后端的思路。

| 参考链接 | 建议关注的内容 |
| --- | --- |
| [DAOS GitHub 仓库](https://github.com/daos-stack/daos) | 完整项目 |
| [官方文件系统文档](https://docs.daos.io/latest/user/filesystem/) | dfuse、libioil、libpil4dfs、预加载方式与限制 |
| [开发版文件系统文档](https://docs.daos.io/master/user/filesystem/) | 拦截访问与 dfuse 缓存一致性等说明 |
| [libioil 源码目录](https://github.com/daos-stack/daos/tree/master/src/client/dfuse/il) | 原始 I/O 拦截库 |
| [il/int_posix.c](https://github.com/daos-stack/daos/blob/master/src/client/dfuse/il/int_posix.c) | fd 管理、dfuse ioctl、分流处理 |
| [il/int_read.c](https://github.com/daos-stack/daos/blob/master/src/client/dfuse/il/int_read.c) | 读操作最终转为 dfs_read |
| [il/ioil_io.h](https://github.com/daos-stack/daos/blob/master/src/client/dfuse/il/ioil_io.h) | 部分公开接口声明 |
| [libpil4dfs 源码目录](https://github.com/daos-stack/daos/tree/master/src/client/dfuse/pil4dfs) | 更广泛的 POSIX 接管 |
| [pil4dfs/int_dfs.c](https://github.com/daos-stack/daos/blob/master/src/client/dfuse/pil4dfs/int_dfs.c) | 挂载点发现、query_path、open/read 分流和 fd 管理 |
| [pil4dfs/hook.c](https://github.com/daos-stack/daos/blob/master/src/client/dfuse/pil4dfs/hook.c) | 共享库函数入口 hook 实现 |
| [DFS 源码目录](https://github.com/daos-stack/daos/tree/master/src/client/dfs) | 可由应用和 FUSE 共用的用户态文件系统接口 |

上述 `master` 和 `latest` 链接会随项目演进变化；正式参考或移植代码时，应记录具体版本和 commit，并检查对应文件的许可证。

## 6. FUSE 补充参考

- [Linux 内核：FUSE I/O Modes](https://kernel.org/doc/html/latest/filesystems/fuse/fuse-io.html)：direct-I/O、缓存读写和 writeback 行为。
- [Linux 内核：FUSE Passthrough](https://kernel.org/doc/html/latest/filesystems/fuse/fuse-passthrough.html)：通过内核直接访问 backing file，绕过用户态守护进程的另一种机制。它与本项目通过预加载库走用户态核心的路径不同；若后端适合映射为内核文件，可作为额外对照。
