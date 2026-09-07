CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
BUILD_DIR ?= $(CURDIR)/build

KERNEL_SRC ?= /home/songlei/project/gcp-dev-baseline/src/linux-6.1.158
KERNEL_BUILD ?= /home/songlei/project/gcp-dev-baseline/build/linux-6.1.158-rt58
KERNEL_CC ?= gcc-12
SYSTEMD_SRC ?= /home/songlei/project/gcp-dev-baseline/src/systemd-252.36

SYSTEMD_CFLAGS ?= -I$(SYSTEMD_SRC)/src
SYSTEMD_LIB ?= /lib/x86_64-linux-gnu/libsystemd.so.0

.PHONY: all user modules stage clean

all: user modules stage

$(BUILD_DIR):
	mkdir -p $@

user: $(BUILD_DIR)/fsctl $(BUILD_DIR)/hb_powerctl_demo $(BUILD_DIR)/libcachemydata.so \
      $(BUILD_DIR)/app_param_test $(BUILD_DIR)/app_param_test_loop

$(BUILD_DIR)/fsctl: src/fsctl.c src/storage.c src/storage.h src/hash.c src/hash.h src/common.c src/common.h src/footer.c src/footer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SYSTEMD_CFLAGS) src/fsctl.c src/storage.c src/hash.c src/common.c src/footer.c \
		-o $@ -lcrypto -lpthread $(SYSTEMD_LIB)

$(BUILD_DIR)/hb_powerctl_demo: src/hb_powerctl_demo.c src/storage.c src/storage.h src/hash.c src/hash.h src/common.c src/common.h src/footer.c src/footer.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SYSTEMD_CFLAGS) src/hb_powerctl_demo.c src/storage.c src/hash.c src/common.c src/footer.c \
		-o $@ -lcrypto -lpthread $(SYSTEMD_LIB)

$(BUILD_DIR)/libcachemydata.so: src/lib/cache_my_data.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fPIC -shared $< -o $@ -ldl

$(BUILD_DIR)/app_param_test: src/test/app_param_test.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/app_param_test_loop: src/test/app_param_test_loop.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

modules: | $(BUILD_DIR)
	$(MAKE) -C $(KERNEL_SRC) O=$(KERNEL_BUILD) M=$(CURDIR)/src/fat CC=$(KERNEL_CC) modules
	cp src/fat/fat.ko src/fat/vfat.ko $(BUILD_DIR)/

stage: user modules
	rm -rf $(BUILD_DIR)/stage
	mkdir -p $(BUILD_DIR)/stage/bin $(BUILD_DIR)/stage/lib $(BUILD_DIR)/stage/modules $(BUILD_DIR)/stage/systemd
	cp $(BUILD_DIR)/fsctl $(BUILD_DIR)/hb_powerctl_demo $(BUILD_DIR)/app_param_test $(BUILD_DIR)/app_param_test_loop $(BUILD_DIR)/stage/bin/
	cp $(BUILD_DIR)/libcachemydata.so $(BUILD_DIR)/stage/lib/
	cp $(BUILD_DIR)/fat.ko $(BUILD_DIR)/vfat.ko $(BUILD_DIR)/stage/modules/
	cp systemd/*.service $(BUILD_DIR)/stage/systemd/

clean:
	$(MAKE) -C $(KERNEL_SRC) O=$(KERNEL_BUILD) M=$(CURDIR)/src/fat CC=$(KERNEL_CC) clean || true
	rm -rf $(BUILD_DIR)
