# SuperRAID A/B 校验、启动恢复与正常下电备份方案

本文描述 `app_param` A/B 分区的完整目标方案。当前已实现代码基线保存在本目录 `patch/`：

- `patch/hbre_power-ctl.patch`：`fsctl`、`libcachemydata.so`、`storage/hash`、`libhbpowerctl.so` 以及基于 Linux 6.1.158 FAT 的二次开发。
- `patch/customer.patch`：产品镜像、systemd unit、App 依赖及部署调整。
- `patch/kernel.patch`：将内核 FAT 构建切换到 `hbre/power-ctl/src/fat`。

上游 Linux / systemd / BusyBox 软件基线参考 `/home/songlei/project/gcp-dev-baseline`。README 只定义最终方案语义；当前实现尚未收敛的内容统一记录在 `TODO.md`。

## 1. 总体结构

产品使用两个固定块分区保存同一份 `app_param` 数据：

```text
A = /dev/mmcblk0p26
B = /dev/mmcblk0p27
mountpoint = /app_param
```

整个方案由五部分组成：

```text
镜像构建
    ↓
FAT32 payload + 4 KiB footer

fsctl + app_param-mount.service
    ↓
启动校验 / 选择 active / mount / 后台修复 peer

libcachemydata.so
    ↓
启动 Recovery 窗口内控制 App 主动持久化

定制 FAT write gate
    ↓
shutdown/reboot 前阻止新的 FAT 修改并完成 sync + flush

libhbpowerctl.so + storage/hash
    ↓
seal active / active → peer / systemd power transition
```

运行生命周期分成三段：

```text
BOOT / RECOVERY
    找到可信 active，尽快启动 App，并恢复 peer

NORMAL
    App 与 FAT 按正常语义运行

SHUTDOWN / REBOOT
    固化 active，更新 footer，复制 peer，再执行电源操作
```

`suspend` 保持原有系统语义，不进入 A/B seal / copy 流程。

## 2. 分区镜像与 footer 格式

A/B 使用 FAT32/VFAT 文件系统。每个分区末尾固定预留 4096 B，不属于 FAT 文件系统有效区域：

```text
partition
┌──────────────────────────────────────┬──────────────┐
│ protected region                     │ footer       │
│ [0, partition_size - 4096)           │ 4096 B       │
└──────────────────────────────────────┴──────────────┘
```

payload 完整性 hash 范围为：

```text
[0, partition_size - sizeof(footer))
```

即 footer 本身不参与 payload hash。

footer 当前协议为 version 1。4096 B footer 默认清零，descriptor 位于最后 128 B。下表 Offset 均相对 descriptor 起始位置：

| Descriptor Offset | Size | 字段 |
| ---: | ---: | --- |
| 0 | 8 | magic：`VFINTEG\0` |
| 8 | 4 | protocol version：`1` |
| 12 | 4 | descriptor size：`128` |
| 16 | 4 | hash algorithm：`SHA-256` |
| 20 | 4 | digest length：`32` |
| 24 | 8 | hashed size：`partition_size - 4096` |
| 32 | 32 | SHA-256 digest |
| 64 | 4 | descriptor CRC32C |
| 68 | 60 | reserved，填 0 |

CRC32C 计算时 descriptor 的 CRC 字段按 0 参与计算。

镜像制作流程为：

```text
创建 FAT32 文件系统
→ 文件系统容量预留末尾 4096 B
→ 写入产品 app_param 文件
→ 计算 protected region SHA-256
→ 生成 footer descriptor
→ 输出完整 partition image
→ A/B 初始部署为相同内容
```

完整分区复制以 partition 为单位，因此复制内容同时包含 FAT 数据区和 footer。

### FAT dirty state

产品完整性和 A/B 选择不依赖 FAT dirty bit。定制 FAT 中 `fat_set_state()` 保持盘上 dirty state 不变，因此 mount/unmount 不会因为 FAT dirty-state 更新主动修改 boot sector。

## 3. systemd 启动关系

启动入口为：

```text
app_param-mount.service
```

目标 unit 语义与当前 patch 一致：

```ini
DefaultDependencies=no
Before=local-fs.target shutdown.target
Conflicts=shutdown.target
Type=notify
RemainAfterExit=yes
WantedBy=sysinit.target
```

启动前加载：

```text
vfat
nls_cp437
nls_iso8859_1
```

核心命令为：

```text
fsctl mount /dev/mmcblk0p26:/dev/mmcblk0p27 /app_param
```

受控 App 通过 `After=` + `Requires=` 依赖 `app_param-mount.service`。当前主要服务包括：

```text
sensor_center.service
video_functions.service
```

这些 App 在启动 Recovery 窗口通过：

```text
LD_PRELOAD=/usr/hobot/lib/libcachemydata.so
```

加载持久化控制库。

## 4. 启动校验与 active 选择

`fsctl` 按固定 A → B 顺序校验候选分区。

每个分区的 verify 包含：

```text
读取 footer descriptor
→ 校验 descriptor CRC32C
→ 校验 magic / version / size / hash algorithm / digest length / hashed_size
→ SHA-256 protected region
→ 与 footer digest 比较
```

选择语义为：

```text
A valid
→ active=A, peer=B

A invalid && B valid
→ active=B, peer=A

A invalid && B invalid
→ app_param-mount.service failed
→ App 不启动
```

找到可信 active 后，`fsctl`：

```text
保存当前 vm.dirty_* 配置
→ 启用 Recovery writeback profile
→ mount active 到 /app_param
→ 记录 active / peer / 启动时可信 digest
→ sd_notify(READY=1)
```

`/app_param` 使用 VFAT，挂载基线为：

```text
MS_NOATIME | MS_NOSUID | MS_NODEV
mount data = NULL
```

`READY=1` 的语义是：已经找到可信 active 并完成 `/app_param` 挂载，依赖该 service 的 App 可以开始启动。peer 修复继续在同一个启动 Recovery 流程中执行。

## 5. 启动 Recovery 的 App 写入模型

Recovery 的目标是让 App 尽快运行，同时保持启动校验得到的物理 active snapshot 可用于修复 peer。

Recovery 窗口允许 App 进行 ordinary buffered write。文件数据和 FAT metadata 先进入内存缓存：

```text
regular file data
→ inode page cache
→ dirty file pages

FAT metadata
→ buffer_head / block-device mapping
→ dirty block pages
```

`libcachemydata.so` 在 constructor 中默认启用 recovery gate，并安装 `SIGUSR2` release handler。

Recovery gate 覆盖 App 常规动态链接持久化入口，包括：

```text
fsync
fdatasync
sync
syncfs
msync
sync_file_range

open/openat/openat2 的 O_DIRECT / O_SYNC / O_DSYNC
fcntl(F_SETFL) 增加 O_DIRECT / O_SYNC / O_DSYNC
```

这些主动持久化操作在 gate 生效期间返回失败；普通 buffered write 继续执行。

App 与该机制的接口约束为：

```text
Recovery 窗口内的业务写入采用 ordinary buffered I/O
```

同时 `fsctl` 使用临时 `vm.dirty_*` Recovery profile，使 Recovery 短窗口内 ordinary buffered file data 和 FAT metadata 尽量保留在 cache 中。profile 的具体参数由目标机实测确定；当前 `0 / 50 / 50` 只作为已验证可工作的起始配置。该 profile 在进入 Recovery 前保存原值，并在 Recovery 完成或失败退出时恢复。

## 6. peer 校验与 Recovery 决策

active 的可信 digest 来自 mount 之前的 verify，记为：

```text
mounted_digest
```

mount 以后不再通过 active partition 自身重新执行 `O_DIRECT` verify。Recovery 只校验 peer：

```text
verify(peer)

peer valid && peer_digest == mounted_digest
→ peer 已经与启动可信 snapshot 一致
→ 跳过复制

peer invalid || peer_digest != mounted_digest
→ 执行 active snapshot → peer 恢复
```

因此 `mounted_digest` 是整个启动 Recovery 的 canonical digest。

## 7. Recovery 源读取与 peer 写入

### 7.1 source：parent block device + partition offset

active 已经作为 FAT 文件系统挂载。Recovery 读取可信物理 snapshot 时，从 parent block device 按 partition 起始偏移读取：

```text
active=A=/dev/mmcblk0p26
parent=/dev/mmcblk0
source_offset=p26_start_sector * 512
```

source fd：

```text
open(parent, O_RDONLY | O_DIRECT | O_CLOEXEC)
```

然后执行：

```text
pread(parent_fd,
      aligned_buffer,
      length,
      source_offset + partition_offset)
```

这样 source Direct I/O 使用 parent bdev mapping，而 mounted FAT 的 block metadata 位于 active partition 自己的 bdev mapping。

### 7.2 destination：peer partition

peer 直接使用 partition block device：

```text
open(peer,
     O_WRONLY/O_RDWR | O_DIRECT | O_CLOEXEC | O_EXCL)
```

copy helper 根据块设备 logical/DMA alignment 分配 aligned buffer。默认传输块可使用 1 MiB：

```text
parent O_DIRECT pread
→ aligned userspace buffer
→ peer O_DIRECT pwrite
```

复制范围为整个 partition：

```text
offset = 0 ... partition_size
```

因此启动时可信 footer 与 protected region 一起复制到 peer。

Direct I/O 的 buffer、offset 和 length 均按实际块设备约束对齐；Direct I/O 失败直接终止本次 Recovery。

### 7.3 Recovery commit 条件

写完 peer 后执行：

```text
fsync(peer)
→ verify(peer)
→ peer_digest == mounted_digest
```

只有 peer 再次完整校验通过，Recovery 才视为完成。

Recovery 的 parent read、peer write 或 post-copy verify 任一步失败时，`fsctl` 记录错误并返回失败，不增加软件层面的自动修复状态机；退出前恢复本轮临时修改的 `vm.dirty_*`。

## 8. Recovery release 与 NORMAL

Recovery 完成条件为：

```text
peer 已经是可信副本
+
受控 App 的 preload gate 已经完成初始化并可接收 release
```

release 顺序为：

```text
1. peer verify PASS
2. 确认需要控制的 App/cachemydata 实例 ready
3. 对这些实例发送 SIGUSR2
4. cachemydata 关闭 recovery gate
5. 恢复启动前保存的 vm.dirty_* 配置
6. 进入 NORMAL
```

NORMAL 阶段：

```text
App ordinary buffered write 正常
App sync/fsync 正常
内核 writeback 正常
FAT metadata update 正常
```

Recovery gate 只服务于启动 A/B 修复窗口。

## 9. startup Recovery 与 shutdown/reboot 的互斥

启动校验、mount、`READY=1`、peer 校验/恢复、恢复 `vm.dirty_*` 和 release `cachemydata` 全部在同一次 `fsctl mount` 执行中完成。

```text
verify / mount
→ READY=1
→ App 开始运行
→ peer verify / Recovery
→ release cachemydata
→ restore vm.dirty_*
→ fsctl mount 返回
```

因此 `fsctl` 自身不需要额外状态来判断 Recovery 是否完成；该执行流在返回前已经知道完整结果。

shutdown/reboot 的 `libhbpowerctl` 是另一条执行路径。目标约束只有一条：当 `fsctl mount` 仍处于 peer Recovery 阶段时，power path 不得同时执行 A/B seal/copy。具体互斥实现保持最小化，不新增独立 Recovery/power-prepare service。

## 10. FAT write gate

定制 FAT 基于 Linux 6.1.158，在 `msdos_sb_info` 中增加：

```c
struct rw_semaphore write_gate;
bool write_blocked;
```

正常运行时：

```text
write_blocked = false
```

会生成新的 FAT 文件系统状态的入口使用：

```text
fat_begin_write(sb)
    ↓
down_read(write_gate)
    ↓
检查 write_blocked
    ↓
执行写操作
    ↓
fat_end_write(sb)
    ↓
up_read(write_gate)
```

当前设计覆盖 regular file write、`MAP_SHARED page_mkwrite`、splice write、fallocate、setattr 以及 VFAT create/unlink/mkdir/rmdir/rename 等写路径。完整目标是所有能够在 gate 关闭后继续产生新 FAT data/metadata 的路径都进入该控制域，或在 disable-write 返回前完成 drain。

### FAT_IOCTL_DISABLE_WRITE

shutdown/reboot 使用产品专用 ioctl：

```text
FAT_IOCTL_DISABLE_WRITE
```

其内核顺序为：

```text
down_write(write_gate)
    ↓
等待已经进入 fat_begin_write() 的 writer 全部退出
    ↓
write_blocked = true
    ↓
down_write(sb->s_umount)
    ↓
sync_filesystem(sb)
    ↓
blkdev_issue_flush(sb->s_bdev)
    ↓
up_write(sb->s_umount)
    ↓
up_write(write_gate)
```

`sync_filesystem()` 的内部 writeback 路径不重新获取 `write_gate` read lock，因此 sync/flush 可以在持有 write-side gate 的情况下完成。

若 sync/flush 失败，`write_blocked` 恢复为 `false` 后 ioctl 返回错误；成功时保持 `write_blocked=true`。

成功返回的目标语义为：

```text
所有 gate 关闭前已经进入的 FAT writer 已完成
+
现有 dirty file data / FAT metadata 已完成 sync
+
block device 已完成 flush
+
后续不会再产生新的 active protected-region 修改
```

此时 active 成为 shutdown/reboot seal 使用的稳定物理 snapshot。

`FAT_IOCTL_DISABLE_WRITE` 不停止、冻结或向 App 发送信号。App 在整个 seal/copy 阶段仍可继续调度和读取 `/app_param`；普通 write/create/rename/setattr 等已纳入 `write_gate` 的 syscall 在 `write_blocked=true` 后返回 `-EROFS`。`MAP_SHARED` 写 fault 通过 `page_mkwrite` 被阻止时表现为 `SIGBUS`，不是 errno 返回。因此“文件系统冻结”在本方案中的语义是“禁止继续改变 active 的盘上状态”，不是停止业务进程。

这个约束也意味着：任何能够在 syscall 写入口结束后延迟到 inode lifecycle 中执行的 FAT metadata 修改，都不能依赖“App 已经退出”来规避，必须由 FAT gate/lifecycle 设计本身覆盖，或由明确的 App I/O 使用约束排除。

## 11. shutdown / reboot 电源流程

产品电源控制链为：

```text
RCore
→ IPC
→ ACore em service
→ libhbpowerctl.so
→ systemd D-Bus
```

`libhbpowerctl.so` 在真正调用 systemd shutdown/reboot 前直接执行 SuperRAID power prepare。

power-prepare 首先根据 `/app_param` 的实际挂载设备得到：

```text
active
peer
```

解析通过 `/app_param` 的 filesystem device number 与 A/B block-device number 比较完成，因此不依赖 mount 时使用的是固定设备节点还是 `by-partlabel` 路径。

然后执行：

```text
FAT_IOCTL_DISABLE_WRITE(/app_param)
    ↓
active sync + flush 完成，write gate 关闭
    ↓
SHA-256 active protected region
    ↓
写 active footer
    ↓
fsync active
    ↓
完整 active → peer
    ↓
fsync peer
    ↓
verify peer
    ↓
systemd D-Bus shutdown/reboot
```

SuperRAID power prepare 的错误不会改变原有电源请求语义。disable-write、seal、copy 或 verify 出错时记录错误并停止继续依赖失败结果，但仍进入原 systemd shutdown/reboot 调用；不尝试在下电请求已经发生后重新打开 `/app_param` 写入或回滚到业务运行态。

`powerctl_seal_block_device()` 对：

```text
[0, partition_size - 4096)
```

计算 SHA-256，然后写入 4096 B footer；footer 写入后执行 `fsync()`。

正常下电完成后的目标状态为：

```text
A == B
```

且两份 footer 都对应最终稳定的 protected region。

### suspend

`suspend` 直接保持原有 powerctl/systemd 调用链：

```text
suspend request
→ systemd suspend
```

它不改变 `/app_param` write gate，不更新 hash/footer，也不执行 A/B copy。

## 12. 关键一致性条件

整个方案以以下条件作为正确性边界：

1. 启动 App 之前至少有一个 footer/hash 校验通过的 active。
2. 启动 Recovery 全程以 mount 前保存的 `mounted_digest` 作为可信 snapshot 标识。
3. peer 只有在 copy、flush、重新 verify 全部通过后才重新成为可信副本；Recovery I/O/verify 失败由 mount service 报错退出。
4. cachemydata gate 只在 peer 恢复完成后 release，随后恢复系统原始 `vm.dirty_*` 配置。
5. shutdown/reboot 的 disable-write ioctl 成功返回后，active protected region 保持稳定，随后执行 seal。
6. shutdown/reboot 始终按 `/app_param` 实际 mount source 执行 `active → peer`。
7. startup Recovery 与 shutdown/reboot 的 A/B 写操作互斥。
8. SuperRAID power prepare 失败不阻断原 systemd shutdown/reboot 请求。
9. suspend 不参与 A/B 一致性更新。
