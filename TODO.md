# SuperRAID TODO

`README.md` 定义最终目标方案；`patch/` 保存原始工程 patch 并只作为参考；后续代码修改只进入 `power-ctl/`。本文件只保留当前初版闭环仍需确认、补充和验证的事项。

GCP A/B 测试上层设备名与板端统一：

```text
A = /dev/mmcblk0p26
B = /dev/mmcblk0p27
```

GCP 直接在测试机上把这两个路径映射到现有 1 GiB `app_param/app_param_bak` 分区；该测试机环境配置不保存在本工程目录。

## 已明确

- [x] **C1 Recovery writeback 的目标语义。**
  - 目标是通过 `vm.dirty_*` 把 Recovery 短窗口内 App 的 ordinary buffered file data 和 FAT metadata 尽量留在 cache 中。
  - `dirty_writeback_centisecs=0 / dirty_background_ratio=50 / dirty_ratio=50` 是当前实测可工作的起始 profile，不是最终参数。
  - 本轮先以该 profile 跑通；后续根据实测 dirty bytes、Recovery 时间和物理 hash 结果再决定是否调整。

- [x] **C2 peer Recovery 失败语义。**
  - peer read / write / verify 出错时 `fsctl` 返回失败，`app_param-mount.service` 进入错误状态并记录日志。
  - 不增加软件层面的 DEGRADED/BLOCKED/自动修复状态机。
  - 失败退出前恢复本轮临时修改的 `vm.dirty_*`。

- [x] **C4 NORMAL 后 App restart/exec 不纳入当前场景。**
  - 产品 App 只考虑一次启动成功或启动失败，本轮不设计 NORMAL 后重新 exec/restart 的 gate 继承。

- [x] **C6 power prepare 后续失败处理。**
  - shutdown/reboot 已经发起后，seal/copy/verify 等 SuperRAID 步骤出错只记录错误，仍继续原有 systemd power transition。
  - 不增加 re-enable write gate / rollback 到业务运行态的逻辑。

- [x] **C8 当前以 J6M 为产品验证目标。**
  - 本机/GCP 初版测试不处理 J6P manifest。
  - GCP 全链路跑通后再同步回正式工程目录和产品 manifest。

- [x] **I1 startup Recovery source 使用 parent block device + partition offset。**
  - `power-ctl/src/storage.c` 已增加 partition → parent/start offset 解析和 range copy。
  - `fsctl` Recovery 已改为 parent `O_DIRECT pread()` → peer `O_DIRECT pwrite()`。

- [x] **I2 Recovery copy 后强制重新 verify peer。**
  - 当前本地实现成功条件为 `copy → fsync(peer) → verify(peer) → peer_digest == mounted_digest`。

- [x] **I10 Recovery release 顺序。**
  - 当前本地实现为 `peer verify → release App cache gate → restore vm.dirty_*`。

- [x] **I13 初版 storage fail-closed 检查。**
  - range copy 已检查 source/destination 不是同一路径、destination size 等于 copy length、range 不越界、Direct I/O 对齐，并对 writable peer 使用 `O_EXCL`。

- [x] **I15 x86_64 GCP CRC32C 构建路径。**
  - descriptor 只有 128 B，`power-ctl/src/hash.c` 已改成 portable CRC32C；SHA-256 继续使用 OpenSSL EVP。

## 已确认问题，处理方案待定

- [ ] **C3 [暂缓] cachemydata release 存在 constructor 时序竞态。**
  - App unit 是 `Type=simple`；systemd 的 `ActiveState=active` 不证明 `libcachemydata.so` constructor 已完成并安装 SIGUSR2 handler。
  - 当前 `READY=1` 后，`fsctl mount` 还会执行 peer hash verify，必要时还会执行完整 Recovery，实际 release 发生得更晚；初版先按现流程测试。
  - 此缺口保留，当前不处理，不阻塞初版闭环。

- [ ] **C5 [暂缓] `fat_evict_inode()` 两个已知边界。**
  - 普通仍有链接文件的普通 `open/write/close` 不会触发这里的 destructive cleanup。
  - 已确认的两个触发条件仅为：`unlink` 已成功但最后引用跨过 disable-write 才释放；或 `FALLOC_FL_KEEP_SIZE` 留下额外 EOF clusters 后在 disable-write 后 eviction。
  - 当前产品正常 I/O 约束下不应出现这两种行为，初版暂不处理；后续如果正式 App 确认使用，再补 lifecycle 方案。

## 当前初版仍需补充

- [x] **I3 用极小 demo 验证 `hb_powerctl_cb()` 新逻辑。**
  - `power-ctl/src/hb_powerctl_demo.c` 按 `/app_param` 实际 mount source 解析 active/peer，使用 `/dev/mmcblk0p26/27`，不再硬编码 A→B。
  - prepare 顺序：resolve → disable-write → seal(active) → copy(active, peer)；下电 copy 后不额外 verify。
  - 正式工程合并时把同样逻辑放回现有 `hb_powerctl_cb()`；当前 patch 只有增量、没有完整 `hb_powerctl.c`，因此测试树使用 demo 代替。

- [x] **I5 按 `SubState` 等待 `fsctl mount` 完整结束。**
  - `hb_powerctl_demo` 在 freeze 前查询 `app_param-mount.service` 的 `SubState`。
  - `start-pre/start/start-post/running` 时等待；只有 `RemainAfterExit=yes` 对应的 `exited` 才进入 disable-write / seal / copy。
  - 这样确认的是同一个 `fsctl mount` 已经完成 peer verify-or-Recovery / release / dirty restore，而不是仅依赖较早的 `READY=1`。
  - 不 stop mount service、不停 App、不新增 service 或锁文件。

- [ ] **I9 [暂缓] 按 C3 实现最小 cachemydata-ready/release。**
  - 当前不处理；初版测试完成后再补 constructor-ready 条件。

- [x] **I11 修复 `vm.dirty_*` 失败清理。**
  - `write_dirty_settings()` 现在三个 sysctl 都会尝试写入，不再因短路 OR 留下半套配置，并保留首个错误返回。
  - 临时 dirty profile 设置失败时立即尝试恢复原配置。
  - 设置成功后，`mkdir`、A/B verify/mount 全失败、状态分配失败、`sd_notify()` 失败都会统一恢复原 dirty 配置再返回。
  - Recovery copy/verify 的既有 `out_restore` 路径保持不变。

- [ ] **I12 [暂缓] `fat_evict_inode()` 两个边界暂不处理。**
  - 对应 C5；正常产品 I/O 不应命中 `unlink 后长期持有最后引用` 或 `FALLOC_FL_KEEP_SIZE` 额外 EOF clusters 这两类行为。
  - 当前不修改 eviction lifecycle，后续只有正式 App 确认使用时再处理。

- [x] **I19 FITRIM 已纳入 FAT write gate。**
  - `fat_ioctl_fitrim()` 现在在 ioctl 入口获取 `fat_begin_write()`，退出时 `fat_end_write()`。
  - disable-write 前已经开始的 FITRIM 会先完成；disable-write 成功后新的 FITRIM 返回 `-EROFS`，不会再从该入口发出 discard。

- [x] **I20 GCP x86_64 `libcachemydata.so` preload 已可启动。**
  - constructor 解析 `pwritev64v2` 时，若 glibc 不导出该符号则退到 64 位等价的 `pwritev2`；其他 ABI 不扩展。
  - 本机 `LD_PRELOAD=... /bin/true` 已通过，不再因缺少 `pwritev64v2` 直接 `_exit(1)`。

- [x] **C9 shutdown/reboot backup 当前不要求 post-copy peer verify。**
  - active 已在 freeze 后 seal 为有效版本；`powerctl_copy_block_device()` 完整复制 active 到 peer 并 `fsync(peer)`，当前初版以 copy/fsync 成功作为下电备份完成条件。
  - startup Recovery 的 copy 后 verify 保持不变。

- [x] **I17 保持原下电流程，只在前面插入 SuperRAID prepare。**
  - 正式 `hb_powerctl_reboot()` 的原 systemd D-Bus shutdown/reboot 调用保持不变。
  - 新增 prepare 在该调用前执行；prepare 内部 fail-fast，但其失败只记录，不 `goto cleanup`，随后仍进入原 systemd D-Bus 调用。
  - `hb_powerctl_demo` 默认模式模拟该语义：prepare 成功/失败后都打印 original power transition continues；`--prepare-only` 仅用于单独测试 prepare 返回值，不实际发起 shutdown/reboot。


## 待测试

### T0. 本地构建与 GCP 部署准备

- [x] **T0.1 编译 `power-ctl/src/fat` 的 `fat.ko` / `vfat.ko`，目标内核为 GCP `6.1.158-rt58`。**
  - 已使用 gcc-12 编译，`fat.ko` / `vfat.ko` vermagic 均为 `6.1.158-rt58 SMP preempt_rt mod_unload modversions`。
- [x] **T0.2 编译 `fsctl`、`hb_powerctl_demo`、`libcachemydata.so`、`app_param_test`、`app_param_test_loop`。**
  - 本地均已构建；`stage` 在完整 `make all` 时生成。
- [ ] **T0.3 在 GCP 直接创建 `/dev/mmcblk0p26/27` 测试 alias，并部署本地已构建的 systemd unit、二进制和 ko。**
  - alias 只属于 GCP 测试机环境，不进入本工程。

- [ ] **T0.4 初始化 A/B 4 KiB VFINTEG footer，并保留可重复恢复的 known-good 镜像。**

### T1. footer / A-B 启动选择

- [ ] **T1.1 seal → verify 正常通过；破坏 protected region 后 verify 失败。**
- [ ] **T1.2 A valid / B valid → mount A。**
- [ ] **T1.3 A valid / B invalid → mount A，恢复 B。**
- [ ] **T1.4 A invalid / B valid → mount B，恢复 A。**
- [ ] **T1.5 A invalid / B invalid → mount service failed，App 不启动。**

### T2. Startup Recovery

- [ ] **T2.1 A→B：parent source offset 全分区 copy，peer fsync + verify PASS。**
- [ ] **T2.2 B→A：与 T2.1 对称。**
- [ ] **T2.3 active 已挂载且 App 持续 ordinary buffered write 时做 Recovery；比较 Recovery 前后 active physical protected-region hash，确认 Recovery source read 没有主动把修改刷入 active。**
- [ ] **T2.4 Recovery read/write/verify 失败注入：`fsctl`/service 报错并恢复临时 dirty profile。**

### T3. cachemydata

- [ ] **T3.1 gate ON：ordinary buffered write 正常，`fsync/fdatasync/sync/syncfs/msync/sync_file_range` 和 `O_DIRECT/O_SYNC/O_DSYNC` 入口被拦截。**
- [ ] **T3.2 SIGUSR2 release 后重复基本接口，恢复真实 libc 行为。**
- [ ] **T3.3 [暂缓] 后续修复 C3 后验证 constructor-ready → release 时序。**
- [ ] **T3.4 [暂缓] 后续处理 release 等待超时/查询失败语义。**

### T4. Recovery writeback profile

- [ ] **T4.1 先以当前 `0 / 50 / 50` profile 重复 Recovery，记录 Recovery 最长时间、窗口内 dirty bytes 和 active physical hash。**
- [ ] **T4.2 如果 T4.1 出现提前 writeback，再基于 Linux writeback 语义调整 sysctl profile并复测；若 T4.1 稳定则初版沿用当前 profile。**

### T5. FAT write gate

- [ ] **T5.1 ordinary write/create/unlink/rename 与 `FAT_IOCTL_DISABLE_WRITE` 并发：等待已有 writer，返回后新写入 `-EROFS`。**
- [ ] **T5.2 App 保持运行时执行 disable-write；确认 App 进程不退出、读取仍可用、普通已覆盖写路径返回 `-EROFS`，同时持续 raw hash active 确认 protected region 不再变化。**
- [ ] **T5.3 [暂缓] open → unlink → disable-write → last close 的 eviction 边界测试。**
- [ ] **T5.4 [暂缓] `FALLOC_FL_KEEP_SIZE` EOF blocks → disable-write → eviction 边界测试。**
- [ ] **T5.5 FITRIM gate 回归：disable-write 前并发 FITRIM 会被 drain；disable-write 返回后新 FITRIM 返回 `-EROFS`。**

### T6. power path

- [ ] **T6.1 active=A：`hb_powerctl_demo --prepare-only` 等到 SubState=exited 后执行 disable-write → seal A → A→B。**
- [ ] **T6.2 active=B：`hb_powerctl_demo --prepare-only` 等到 SubState=exited 后执行 disable-write → seal B → B→A。**
- [ ] **T6.3 Recovery 正在执行时启动 demo：确认 SubState=running 时等待，`fsctl mount` 完成进入 exited 后才开始 freeze/copy；App 保持运行。**
- [ ] **T6.4 suspend 在正式工程仍直接走原 powerctl suspend 路径，不进入新增 prepare。**
- [ ] **T6.5 demo 默认模式验证 prepare 失败不改变“原 power transition continues”的控制流语义；正式工程保持原 systemd D-Bus 调用不变。**

### T7. 初版端到端

- [ ] **T7.1 连续 reboot 多轮：boot verify → Recovery → App write → shutdown seal/copy → next boot verify。**
- [ ] **T7.2 交替制造 A invalid / B invalid，验证两方向自动恢复。**

## 后续优化，不阻塞初版

- [ ] generation/version/split-brain 仲裁。
- [ ] 产品 ABI 中若实际出现 `RWF_SYNC/RWF_DSYNC`，再决定 `pwritev2/pwritev64v2` wrapper；当前实际工程只有 `pwritev64v2` 且无对应 RWF UAPI 使用。
- [ ] `open64/openat64/fcntl64` 等额外 ABI alias，仅在正式产品二进制确认存在实际引用后补。
- [ ] `vm.dirty_bytes/dirty_background_bytes` 与 ratio 模式的完整产品化保存/恢复。
- [ ] Direct I/O alignment 从当前 GCP/J6M 已知布局推广到其他逻辑块大小设备。
- [ ] J6P manifest 和其他产品变体集成。
- [ ] 正式 image build pipeline 的 footer 自动 seal/verify 与 customer patch 测试脚手架清理。
