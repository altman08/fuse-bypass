# DAOS 源码依据与 demo 裁剪

固定参考 commit：`dcb0e18cf66264857a78b31e1e96c4d0648b08af`（2026-11-17）。以下结论来自该版本源码，不把 DAOS 专用机制描述成通用 FUSE 能力。

## 分流依据

| DAOS 位置 | 源码行为 | 本 demo 的对应实现 |
| --- | --- | --- |
| [pil4dfs query_dfs_mount](https://github.com/daos-stack/daos/blob/dcb0e18cf66264857a78b31e1e96c4d0648b08af/src/client/dfuse/pil4dfs/int_dfs.c#L576) | 已登记挂载路径的最长匹配，要求后继字符为 `/` 或字符串结束 | 显式配置一个目标挂载根，使用同样的目录边界判断；不猜测所有 FUSE/NFS 挂载都是目标 |
| [discover_dfuse_mounts / discover_daos_mount_with_env](https://github.com/daos-stack/daos/blob/dcb0e18cf66264857a78b31e1e96c4d0648b08af/src/client/dfuse/pil4dfs/int_dfs.c#L596) | 从 `/proc/self/mounts` 的 `fuse.daos` 类型或环境配置取得 DAOS 挂载 | 没有 DAOS 身份协议，使用 `FUSE_BYPASS_MOUNT` 显式登记与编入的目标代码对应的路径 |
| [query_path](https://github.com/daos-stack/daos/blob/dcb0e18cf66264857a78b31e1e96c4d0648b08af/src/client/dfuse/pil4dfs/int_dfs.c#L1141) | 解析相对路径、规范化、匹配挂载、延迟初始化 DFS；部分路径交给内核处理 | 首版只接管规范绝对路径；非规范路径在打开前整文件交给 libc。不擅自折叠符号链接前的 `..` |
| [open_common](https://github.com/daos-stack/daos/blob/dcb0e18cf66264857a78b31e1e96c4d0648b08af/src/client/dfuse/pil4dfs/int_dfs.c#L2240) | 非目标进入原函数；普通模式为 DFS 对象分配用户态 fd；兼容模式使用内核 fd + ioctl | 使用普通模式的用户态 fd 与打开状态表；将 DFS 打开替换成用户 `fuse_operations.open/create` |
| [d_get_fd_redirected / read_comm / pread](https://github.com/daos-stack/daos/blob/dcb0e18cf66264857a78b31e1e96c4d0648b08af/src/client/dfuse/pil4dfs/int_dfs.c#L1947) | 根据 fd 表和重定向决定调用 DFS 或原 libc，顺序 I/O 更新偏移 | 表内 fd 调用用户回调，非目标 fd 原样转发；不支持 dup 重定向 |
| [ioil dfuse_open](https://github.com/daos-stack/daos/blob/dcb0e18cf66264857a78b31e1e96c4d0648b08af/src/client/dfuse/il/int_posix.c#L1098) / [check_ioctl_on_open](https://github.com/daos-stack/daos/blob/dcb0e18cf66264857a78b31e1e96c4d0648b08af/src/client/dfuse/il/int_posix.c#L845) | 真实 open 后调用 `DFUSE_IOCTL_IL` 检查版本并获取 DAOS 信息；失败保留内核路径 | 不采用：普通 FUSE 文件系统没有 dfuse 专有协议，且无法无修改地增加该协议 |

## 拦截范围

拦截的是动态链接的 libc/POSIX 函数符号，不是内核系统调用表。DAOS 的 `libpil4dfs` 还通过 [register_a_hook](https://github.com/daos-stack/daos/blob/dcb0e18cf66264857a78b31e1e96c4d0648b08af/src/client/dfuse/pil4dfs/int_dfs.c#L7564) 对 libc/libpthread 函数入口打补丁；本 demo 只实现 LD_PRELOAD 符号替换，不声称能覆盖 libc 内部隐藏调用或直接 syscall。

| 本 demo 入口 | DAOS 依据 | 回调或行为 |
| --- | --- | --- |
| `open/open64/creat/creat64`、fortify open 入口 | `open_common`、`new_open_libc` | `getattr` 检查类型与路径组件，`open/create`、必要时 `truncate` |
| `close` | `new_close_common` | `flush/release`，销毁表项 |
| `read/write` | `read_comm/write_comm` | `read/write` 回调，加当前偏移 |
| `pread/pread64/pwrite/pwrite64` | `pread/pwrite` | 带显式偏移，不修改当前偏移 |
| `lseek/lseek64` | `lseek_comm` | 管理偏移，SEEK_END 使用 `getattr` |
| `fsync/fdatasync` | 同名函数 | 调用 `ops.fsync(path, datasync, fi)`；缺失则报错 |

DAOS 还覆盖 `openat`、向量 I/O、stat、目录、链接/重命名、dup/fcntl、mmap 等接口。本 demo 不宣称支持这些接口在用户态 fd 上的语义，测试程序不得使用它们；内核无法识别用户态 fd，未拦截的 fd 系统调用通常返回 EBADF。目标目录操作和不支持的打开标志在 open 阶段选择整文件走 libc，不先创建后端句柄再混用两条路径。

## 明确的适配差异

- DAOS 调用 DFS/DAOS API；本项目按要求调用编入的用户 libfuse3 回调。用户源文件以编译参数重命名 `main`，标准 `fuse_main_real` 入口由本项目接管，不要求用户提供插件导出函数。
- DAOS 的环境变量、mount 类型、pool/container、ioctl 和对象格式都是专有协议，不照搬为通用识别机制。
- 使用 DAOS 普通模式的用户态 fd 思路，但启动时验证所选 fd 区间高于系统可分配的 fd 上界，避免与真实 fd 冲突。
- DAOS 的同步函数在部分模式下直接成功；这依赖其后端语义，不能照搬到任意 FUSE 文件系统。这里必须调用用户同步回调。
- 首版是单进程受控测试；不实现 DAOS 的二进制入口 patch、fork/exec 支持、dup 映射和缓存一致性机制。
- 目标符号链接和嵌套挂载不走 bypass；回调需要在同一应用进程中使用当前调用者身份正确执行权限检查。内核的 `default_permissions` 不会替直接回调执行检查。

实现已直接移植 `query_dfs_mount`、`read_comm`、`write_comm`，保留原函数名和主要分流逻辑；其他入口作了 FUSE 适配。复制范围与修改记录见 [NOTICE](../third_party/daos/NOTICE.md)，原 BSD-2-Clause-Patent 许可证及版权声明已保留。
