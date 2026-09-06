# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux fat filesystem support.

ifeq ($(KERNELRELEASE),)

# Kbuild writes external-module output beside M=; stage it in the output tree.
FAT_OUT := $(OUTPUT_BUILD_DIR)/hbre/power-ctl/fat

.PHONY: all modules install clean stage-sources

all: install

modules: stage-sources
	$(MAKE) -C $(SRC_KERNEL_DIR) O=$(OUTPUT_BUILD_DIR)/kernel M=$(FAT_OUT) \
		ARCH=$(ARCH_KERNEL) CROSS_COMPILE=$(CROSS_COMPILE) modules

install: modules
	$(MAKE) -C $(SRC_KERNEL_DIR) O=$(OUTPUT_BUILD_DIR)/kernel M=$(FAT_OUT) \
		ARCH=$(ARCH_KERNEL) CROSS_COMPILE=$(CROSS_COMPILE) \
		INSTALL_MOD_PATH=$(DEPLOY_ROOTFS_DEV_DIR) \
		INSTALL_MOD_DIR=kernel/fs/fat modules_install

stage-sources:
	@rm -rf $(FAT_OUT)
	@mkdir -p $(FAT_OUT)
	@cp -f *.c *.h Makefile $(FAT_OUT)/

clean:
	rm -rf $(FAT_OUT)

else

# The kernel/fs/Makefile still references this directory, but the normal
# kernel build must not compile FAT.  Only the external M= build emits it.
ifneq ($(KBUILD_EXTMOD),)
obj-m += fat.o
obj-m += vfat.o

fat-y := cache.o dir.o fatent.o file.o inode.o misc.o nfs.o
vfat-y := namei_vfat.o
endif

endif
