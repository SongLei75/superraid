# power-ctl

本目录包含 app_param A/B 校验、恢复、下电封存及 FAT 写入门控相关代码。

主要模块：

- `src/common.*`：跨模块通用基础能力，如字节序编解码、完整 pread/pwrite、fd 大小查询。
- `src/hash.*`：SHA-256 与 CRC32C 计算。
- `src/footer.*`：footer descriptor 的读取、校验与更新。
- `src/storage.*`：块设备、挂载、恢复、复制及下电准备相关逻辑。
- `src/lib/cache_my_data.c`：Recovery 阶段对显式 sync/direct 类接口进行门控的 preload 库。
- `src/fat/`：带写入门控扩展的 FAT/VFAT 模块。
- `src/test/app_param_test.c`：App 写入行为测试程序。
- `systemd/`：app_param 挂载及测试 App service 示例。

## Standalone build

用户态默认通过系统 `pkg-config` 查找 libsystemd。
内核模块默认使用当前运行内核的 build tree，也可显式传入产品工程参数：

```bash
make \
  KERNEL_SRC=/path/to/kernel/source \
  KERNEL_BUILD=/path/to/kernel/build \
  KERNEL_CC=aarch64-linux-gnu-gcc
```

如系统无法通过 `pkg-config` 找到 libsystemd，可显式指定：

```bash
make \
  SYSTEMD_CFLAGS='-I/path/to/systemd/include' \
  SYSTEMD_LIB='-lsystemd'
```


## fsctl

```text
fsctl seal DEVICE
fsctl verify DEVICE
fsctl mount A:B MOUNTPOINT
fsctl umount A:B MOUNTPOINT
```

`umount` 是下电准备入口：冻结已挂载 FAT 的后续写入，更新 active descriptor，随后完整复制 active 到 peer；它不执行传统 Linux `umount(2)`。
