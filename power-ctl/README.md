# power-ctl local test tree

本目录用于把 `patch/` 中的 SuperRAID 关键代码还原成可直接构建并部署到 GCP 的测试工程。

## Source baseline

- `src/fat/`：基线来自 `/home/songlei/project/gcp-dev-baseline/src/linux-6.1.158/fs/fat`，随后按 `patch/hbre_power-ctl.patch` 中完整 `src/fat/*` 内容还原。
- `src/fsctl.c`、`src/storage.[ch]`、`src/hash.[ch]`、`src/lib/cache_my_data.c`、测试程序：由 `patch/hbre_power-ctl.patch` 还原。
- `systemd/`：GCP 初版测试 unit，A/B 上层路径与板端统一为 `/dev/mmcblk0p26`、`/dev/mmcblk0p27`。GCP 的设备 alias 直接部署在测试机，不保存在本目录。

## Latest target changes applied locally

相对原始 patch，本目录已经按当前 README/TODO 目标补入：

1. startup Recovery 从 active partition 的 parent block device + partition start offset 做 `O_DIRECT pread()`。
2. peer copy 后执行 `fsync + verify`，并要求 digest 等于启动时 `mounted_digest`。
3. Recovery release 顺序改为 App gate release 后恢复 `vm.dirty_*`；Recovery 失败也恢复临时 dirty profile。
4. block range copy 对 peer 使用 `O_EXCL`，并检查 size/range/alignment。
5. CRC32C 改为 portable software implementation，便于 x86_64 GCP 构建；SHA-256 仍使用 OpenSSL EVP。
6. `powerctl_resolve_active_peer()` 根据 `/app_param` 的 `st_dev` 与 A/B `st_rdev` 解析真实 active/peer，供现有 `libhbpowerctl` power path 接入。
7. startup Recovery 仍在单个 `app_param-mount.service/fsctl` 中完成；`fsctl mount` 自身同步知道 Recovery 完成点。power path 的互斥只在现有 `fsctl` / `libhbpowerctl` 两条执行路径之间补最小机制，不新增 service。

尚未实现的目标项以根目录 `TODO.md` 为准，当前主要是 C3、C5、I9、I11/I12 的剩余部分和 GCP 实机验证。

## Build

```bash
make
```

默认使用：

```text
kernel source = /home/songlei/project/gcp-dev-baseline/src/linux-6.1.158
kernel build  = /home/songlei/project/gcp-dev-baseline/build/linux-6.1.158-rt58
kernel CC     = gcc-12
```

输出集中在：

```text
build/stage/bin/
build/stage/lib/
build/stage/modules/
build/stage/systemd/
```

`fat.ko` / `vfat.ko` 的目标 vermagic 为 `6.1.158-rt58 SMP preempt_rt mod_unload modversions`。
