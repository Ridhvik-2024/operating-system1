# AI-OS M9/v1.0 — boot Makefile
#
# Targets:
#   make -f boot/Makefile boot-image   build aios-initramfs.cpio.xz (+ kernel symlink)
#   make -f boot/Makefile boot-run     boot the image in QEMU (interactive serial console)
#   make -f boot/Makefile boot-test    headless boot test; asserts selftest PASS
#
# Toolchain expectations (fetched once by boot/fetch.sh):
#   boot/dl/kernel/boot/vmlinuz-virt   Linux kernel bzImage (Alpine linux-virt 6.6)
#   boot/dl/busybox-root/bin/busybox.static
#   boot/dl/modules/{virtio_blk,mbcache,ext2}.ko   M8 storage stack
#
# QEMU is used from $AIOS_QEMU if set (e.g. a locally extracted build),
# otherwise plain `qemu-system-x86_64` from PATH.

CC      ?= gcc
# image build: section-level GC keeps every static binary lean (the hard
# size cap from m6 forces each binary to carry only what it calls)
CFLAGS  ?= -std=c11 -Os -Wall -Wextra -Wshadow -D_GNU_SOURCE \
	   -ffunction-sections -fdata-sections
LDFLAGS := -static -pthread -lm -Wl,--gc-sections

LIB_SRCS  := src/json.c src/types.c src/aiproc.c src/sembus.c \
	     src/action.c src/policy.c src/mem.c src/tools.c src/sandbox.c \
	     src/stage.c src/effects.c \
	     src/agent.c \
	     src/cortex.c src/shell.c src/shroute.c src/ksched.c src/ebpf.c src/hardening.c \
	     src/microlm.c src/autopilot.c src/durable.c \
	     src/fb.c src/win.c src/input.c
LIB       := boot/build/libaios.a
TEST_BINS := $(patsubst tests/%.c,boot/build/%,$(wildcard tests/test_*.c))
# The M4 adversary ships with the image: test_shell and test_effects run inside
# the booted system, and the image has no compiler to build it there.
FIXTURE_BINS := boot/build/liar-worker boot/build/deaf-worker

BOOT_DIR  := boot
BLD       := boot/build
STAGE     := $(BLD)/rootfs
KERNEL    := $(BOOT_DIR)/dl/kernel/boot/vmlinuz-virt
BUSYBOX   := $(BOOT_DIR)/dl/busybox-root/bin/busybox.static
MODULES   := $(wildcard $(BOOT_DIR)/dl/modules/*.ko)
MKCPIO    := $(BLD)/mkcpio
INITRD    := $(BLD)/aios-initramfs.cpio.xz
QEMU      ?= qemu-system-x86_64

.PHONY: boot-image boot-run boot-test clean

boot-image: $(INITRD) boot/run-qemu.sh boot/boot-test.sh
	@echo "== AI-OS v1.1 (M10 desktop) boot image ready =="
	@echo "   kernel:   $(KERNEL)"
	@echo "   initramfs: $(INITRD)"
	@ls -la $(INITRD) $(KERNEL)

LIB_OBJS := $(patsubst src/%.c,$(BLD)/%.o,$(LIB_SRCS))

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^

# M10: the 8x16 font table is src/font8x16.h, committed (see the host
# Makefile). No Python, no Pillow, no DejaVu fonts in the image build.
$(BLD)/%.o: src/%.c $(wildcard include/aios/*.h) $(wildcard src/*.h)
	@mkdir -p $(BLD)
	$(CC) $(CFLAGS) -Iinclude -MMD -c -o $@ $<

$(BLD)/aios-init: $(BOOT_DIR)/init.c $(LIB)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(LIB) $(LDFLAGS)

$(BLD)/probe: $(BOOT_DIR)/probe.c
	$(CC) -O2 -Wall -static -o $@ $<

# v1.0: probe is no longer shipped in the image (an M5.5 leftover nothing
# at runtime called); its source stays here for dev-time use only.

$(BLD)/aios_intent.ko: $(BOOT_DIR)/module/aios_intent.c $(BOOT_DIR)/module/Kbuild \
	               $(BOOT_DIR)/module/build.sh
	@mkdir -p $(BLD)
	@rm -f $(BOOT_DIR)/module/aios_intent.ko
	cd $(BOOT_DIR)/module && bash build.sh >/dev/null
	@cp $(BOOT_DIR)/module/aios_intent.ko $@

$(BLD)/%: apps/%.c $(LIB)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(LIB) $(LDFLAGS)

$(BLD)/tool-exec: src/tool-exec.c $(LIB)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(LIB) $(LDFLAGS)

$(BLD)/aios-governor: apps/aios-governor.c $(LIB)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(LIB) $(LDFLAGS)

$(BLD)/test_%: tests/test_%.c $(LIB) tests/t.h
	@mkdir -p $(BLD)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(LIB) $(LDFLAGS)

$(MKCPIO): $(BOOT_DIR)/mkcpio.c
	@mkdir -p $(BLD)
	$(CC) -O2 -Wall -o $@ $<

GPUMODS := $(wildcard $(BOOT_DIR)/dl/modules-gpu/*.ko.gz)
ASSETPNG := $(wildcard desktop/assets/*.png)
ASSETS := $(patsubst desktop/assets/%.png,$(BLD)/assets/%.aiosimg,$(ASSETPNG))

$(BLD)/assets/%.aiosimg: desktop/assets/%.png tools/img2aiosimg.py
	@mkdir -p $(BLD)/assets
	python3 tools/img2aiosimg.py $< $@

# A test fixture, built by the same toolchain as everything else and installed
# next to the tests it belongs to.
boot/build/liar-worker: tests/fixtures/liar-worker.c
	@mkdir -p $(BLD)
	$(CC) $(CFLAGS) -o $@ $<

boot/build/deaf-worker: tests/fixtures/deaf-worker.c
	@mkdir -p $(BLD)
	$(CC) $(CFLAGS) -o $@ $<

$(INITRD): $(MKCPIO) $(BLD)/aios-init $(BLD)/aiosh \
	   $(BLD)/aios-memctl $(BLD)/tool-exec $(BLD)/aios-governor \
	   $(BLD)/aios-desktop $(TEST_BINS) $(FIXTURE_BINS) \
	   $(BOOT_DIR)/aios-selftest.sh $(BOOT_DIR)/boot.aiosh \
	   $(BOOT_DIR)/devnodes.txt $(KERNEL) $(BUSYBOX) $(BLD)/aios_intent.ko \
	   $(MODULES) $(GPUMODS) $(ASSETS)
	@echo "== staging AI-OS rootfs =="
	@rm -rf $(STAGE)
	@mkdir -p $(STAGE)/bin $(STAGE)/sbin $(STAGE)/usr/lib/aios/tests \
	          $(STAGE)/usr/bin $(STAGE)/etc/aios $(STAGE)/proc $(STAGE)/sys \
	          $(STAGE)/dev $(STAGE)/tmp $(STAGE)/root $(STAGE)/var/lib/aios \
	          $(STAGE)/var/aios-selftest $(STAGE)/lib/modules
	@install -s -m 0755 $(BLD)/aios-init   $(STAGE)/init
	@install -m 0644 $(BLD)/aios_intent.ko $(STAGE)/lib/modules/aios_intent.ko
	@install -m 0755 $(BUSYBOX)         $(STAGE)/bin/busybox
	@ln -sf busybox $(STAGE)/bin/sh
	@ln -sf busybox $(STAGE)/bin/mount
	@ln -sf busybox $(STAGE)/bin/mkdir
	@ln -sf busybox $(STAGE)/bin/echo
	@ln -sf busybox $(STAGE)/bin/ls
	@ln -sf busybox $(STAGE)/bin/cat
	@ln -sf busybox $(STAGE)/bin/ps
	@ln -sf busybox $(STAGE)/bin/reboot
	@ln -sf busybox $(STAGE)/bin/poweroff
	@ln -sf busybox $(STAGE)/bin/halt
	@ln -sf busybox $(STAGE)/bin/sync
	@ln -sf busybox $(STAGE)/bin/hostname
	@ln -sf busybox $(STAGE)/bin/rm
	@ln -sf busybox $(STAGE)/bin/cp
	@ln -sf busybox $(STAGE)/bin/mv
	@ln -sf busybox $(STAGE)/bin/basename
	@ln -sf busybox $(STAGE)/bin/sed
	@ln -sf busybox $(STAGE)/bin/head
	@ln -sf busybox $(STAGE)/bin/tail
	@ln -sf busybox $(STAGE)/bin/grep
	@ln -sf busybox $(STAGE)/bin/wc
	@ln -sf busybox $(STAGE)/bin/sort
	@ln -sf busybox $(STAGE)/bin/uniq
	@ln -sf busybox $(STAGE)/bin/dirname
	@ln -sf busybox $(STAGE)/bin/touch
	@ln -sf busybox $(STAGE)/bin/sleep
	@ln -sf busybox $(STAGE)/bin/true
	@ln -sf busybox $(STAGE)/bin/false
	@ln -sf busybox $(STAGE)/bin/insmod
	@install -s -m 0755 $(BLD)/aiosh      $(STAGE)/sbin/aiosh
	@install -s -m 0755 $(BLD)/aios-memctl $(STAGE)/sbin/aios-memctl
	@install -s -m 0755 $(BLD)/tool-exec  $(STAGE)/sbin/tool-exec
	@install -s -m 0755 $(BLD)/aios-governor $(STAGE)/sbin/aios-governor
	@install -s -m 0755 $(BLD)/aios-desktop $(STAGE)/sbin/aios-desktop
	@install -m 0644 $(MODULES) $(STAGE)/lib/modules/
	@install -s -m 0755 $(TEST_BINS)      $(STAGE)/usr/lib/aios/tests/
	@install -s -m 0755 $(FIXTURE_BINS)  $(STAGE)/usr/lib/aios/tests/
	@install -m 0755 $(BOOT_DIR)/aios-selftest.sh $(STAGE)/usr/bin/aios-selftest
	@install -m 0644 $(BOOT_DIR)/boot.aiosh $(STAGE)/etc/aios/boot.aiosh
	@mkdir -p $(STAGE)/usr/lib/aios/gpu $(STAGE)/usr/lib/aios/assets
	@install -m 0644 $(GPUMODS) $(STAGE)/usr/lib/aios/gpu/
	@if [ -n "$(ASSETS)" ]; then install -m 0644 $(ASSETS) \
	    $(STAGE)/usr/lib/aios/assets/; fi
	@cp -r workspace $(STAGE)/workspace
	@ver=$$(sed -n 's/^#define AIOS_VERSION "\(.*\)"/\1/p' include/aios/types.h); \
	 printf 'NAME="AI-OS"\nVERSION="%s (v1.1 desktop)"\nID=aios\nPRETTY_NAME="AI-OS %s (AI-native on Linux, M10 desktop)"\n' "$$ver" "$$ver" > $(STAGE)/etc/os-release
	@printf 'aios\n' > $(STAGE)/etc/hostname
	@echo "== packing initramfs (xz: the kernel decompresses it, RD_XZ=y) =="
	$(MKCPIO) $(STAGE) $(BOOT_DIR)/devnodes.txt | xz -9 --check=crc32 -T1 > $@
	@du -h $@
	@echo "== size guard (M10 documented raise) =="
	@echo "   history: m6 cap 5.0MB -> m7 2.29MB -> m8 1.64MB -> m9 = m8 actual"
	@echo "   M10 raise: GPU/input module stack (16 gz modules, ~845KB) ships"
	@echo "   once; new cap = first m10 image size + 128KB headroom, pinned"
	@echo "   in boot/.m10-size-cap"
	@if [ ! -f $(BOOT_DIR)/.m10-size-cap ]; then \
	     s=$$(stat -c %s $@); echo $$((s + 131072)) > $(BOOT_DIR)/.m10-size-cap; \
	     echo "   cap pinned: $$((s + 131072)) (first build + 128KB headroom)"; \
	     echo "   this file is tracked in git, so raising it is a reviewed commit:"; \
	     echo "   that is the only form of this guard that can fail in CI"; \
	 fi; \
	 echo "   (the cap is toolchain-dependent: a static link against a newer" ; \
	 echo "    glibc is larger. Pin it from the LARGEST supported toolchain," ; \
	 echo "    not from the smallest.)" ; \
	 cap=$$(cat $(BOOT_DIR)/.m10-size-cap); size=$$(stat -c %s $@); \
	 echo "   v1.1 initramfs: $$size bytes (cap $$cap)"; \
	 if [ $$size -gt $$cap ]; then \
	     echo "SIZE-GUARD FAIL: v1.1 image ($$size) exceeds M10 cap ($$cap)"; \
	     exit 1; \
	 fi; \
	 echo "   size-guard OK ($$((cap - size)) bytes under cap)"

boot-run: $(INITRD) boot/run-qemu.sh
	bash boot/run-qemu.sh

boot-test: $(INITRD) boot/boot-test.sh
	bash boot/boot-test.sh

clean:
	rm -rf $(BLD)
