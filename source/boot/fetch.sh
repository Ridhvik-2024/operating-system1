#!/bin/bash
# fetch.sh — re-download the boot toolchain (kernel + static busybox +
# kernel headers for the LKM) if boot/dl is missing. Everything else is
# built from source by the Makefiles. No root required.
set -euo pipefail
cd "$(dirname "$0")"   # aios/boot

BASE=https://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64
KERNEL_APK=linux-virt-6.6.142-r0.apk
DEV_APK=linux-virt-dev-6.6.142-r0.apk
DEV_APK_SHORT=linux-virt-dev.apk      # name the apk is committed under
BUSYBOX_APK=busybox-static-1.36.1-r31.apk

mkdir -p dl
cd dl

if [ ! -f kernel/boot/vmlinuz-virt ]; then
    echo "[fetch] downloading kernel ($KERNEL_APK, ~38 MB)..."
    curl -fL -o "$KERNEL_APK" "$BASE/$KERNEL_APK"
    mkdir -p kernel
    tar xzf "$KERNEL_APK" -C kernel boot/vmlinuz-virt 2>/dev/null || true
    rm -f "$KERNEL_APK"
    echo "[fetch] kernel ready: kernel/boot/vmlinuz-virt"
fi

if [ ! -f modules/ext2.ko ]; then
    # M8 durable: ext2 + its deps (mbcache, virtio_blk) from the same
    # kernel build; extracted uncompressed so the initramfs compressor
    # packs them once.
    echo "[fetch] downloading modules ($KERNEL_APK, ~38 MB)..."
    curl -fL -o "$KERNEL_APK" "$BASE/$KERNEL_APK"
    mkdir -p modules modsrc
    tar xzf "$KERNEL_APK" -C modsrc \
        lib/modules/6.6.142-0-virt/kernel/fs/ext2/ext2.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/fs/mbcache.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/block/virtio_blk.ko.gz \
        2>/dev/null || true
    zcat modsrc/lib/modules/6.6.142-0-virt/kernel/fs/ext2/ext2.ko.gz > modules/ext2.ko
    zcat modsrc/lib/modules/6.6.142-0-virt/kernel/fs/mbcache.ko.gz > modules/mbcache.ko
    zcat modsrc/lib/modules/6.6.142-0-virt/kernel/drivers/block/virtio_blk.ko.gz > modules/virtio_blk.ko
    rm -rf modsrc "$KERNEL_APK"
    echo "[fetch] modules ready: modules/{ext2,mbcache,virtio_blk}.ko"
fi

if [ ! -f busybox-root/bin/busybox.static ]; then
    echo "[fetch] downloading busybox-static (~0.6 MB)..."
    curl -fL -o "$BUSYBOX_APK" "$BASE/$BUSYBOX_APK"
    mkdir -p busybox-root
    tar xzf "$BUSYBOX_APK" -C busybox-root 2>/dev/null || true
    rm -f "$BUSYBOX_APK"
    echo "[fetch] busybox ready: busybox-root/bin/busybox.static"
fi

# Sentinel must be a file that actually ships. It used to test for
# virtio-gpu.ko.gz, which is NOT in the set (the stack is bochs + evdev +
# virtio_input), so this block re-downloaded 38 MB on every run - including
# in CI, where it failed on a network hiccup despite everything being
# committed. Check the whole expected set instead.
GPU_MODS="bochs drm drm_kms_helper drm_panel_orientation_quirks drm_shmem_helper
drm_ttm_helper drm_vram_helper evdev fb fb_io_fops fb_sys_fops fbdev i2c-core
syscopyarea sysfillrect sysimgblt ttm virtio_input"
gpu_missing=0
for m in $GPU_MODS; do
    [ -f "modules-gpu/$m.ko.gz" ] || gpu_missing=1
done
if [ "$gpu_missing" = 1 ]; then
    # M10 desktop: the DRM/bochs + evdev module stack (still gzipped —
    # 743 KB shipped; aios-init gunzips + insmods them at boot when the GUI
    # is enabled). Order proven by boot/dl/modules-gpu load order in init.c.
    echo "[fetch] downloading GPU/input modules ($KERNEL_APK, ~38 MB)..."
    curl -fL -o "$KERNEL_APK" "$BASE/$KERNEL_APK"
    mkdir -p modules-gpu gpu-modsrc
    tar xzf "$KERNEL_APK" -C gpu-modsrc \
        lib/modules/6.6.142-0-virt/kernel/drivers/video/fbdev/core/sysfillrect.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/video/fbdev/core/syscopyarea.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/video/fbdev/core/sysimgblt.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/video/fbdev/core/fb_sys_fops.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/video/fbdev/core/fb_io_fops.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/arch/x86/video/fbdev.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/video/fbdev/core/fb.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/i2c/i2c-core.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/gpu/drm/drm_panel_orientation_quirks.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/gpu/drm/drm.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/gpu/drm/drm_kms_helper.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/gpu/drm/drm_shmem_helper.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/gpu/drm/ttm/ttm.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/gpu/drm/drm_ttm_helper.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/gpu/drm/drm_vram_helper.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/gpu/drm/tiny/bochs.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/input/evdev.ko.gz \
        lib/modules/6.6.142-0-virt/kernel/drivers/virtio/virtio_input.ko.gz \
        2>/dev/null || true
    (cd gpu-modsrc/lib/modules/6.6.142-0-virt/kernel \
     && cp drivers/video/fbdev/core/sysfillrect.ko.gz drivers/video/fbdev/core/syscopyarea.ko.gz \
           drivers/video/fbdev/core/sysimgblt.ko.gz drivers/video/fbdev/core/fb_sys_fops.ko.gz \
           drivers/video/fbdev/core/fb_io_fops.ko.gz arch/x86/video/fbdev.ko.gz \
           drivers/video/fbdev/core/fb.ko.gz drivers/i2c/i2c-core.ko.gz \
           drivers/gpu/drm/drm_panel_orientation_quirks.ko.gz drivers/gpu/drm/drm.ko.gz \
           drivers/gpu/drm/drm_kms_helper.ko.gz drivers/gpu/drm/drm_shmem_helper.ko.gz \
           drivers/gpu/drm/ttm/ttm.ko.gz drivers/gpu/drm/drm_ttm_helper.ko.gz \
           drivers/gpu/drm/drm_vram_helper.ko.gz drivers/gpu/drm/tiny/bochs.ko.gz \
           drivers/input/evdev.ko.gz drivers/virtio/virtio_input.ko.gz \
           ../../../../../../../modules-gpu/)
    rm -rf gpu-modsrc "$KERNEL_APK"
    echo "[fetch] GPU modules ready: modules-gpu/*.ko.gz (18 modules)"
fi

if [ ! -f kern-dev/usr/src/linux-headers-6.6.142-0-virt/Makefile ]; then
    mkdir -p kern-dev
    # The dev apk is COMMITTED (18 MB) so extracting it needs no network.
    # It is stored under the unversioned name; older checkouts used the
    # upstream versioned name, so accept both before downloading anything.
    dev_src=""
    for cand in "$DEV_APK" "$DEV_APK_SHORT"; do
        if [ -f "$cand" ]; then dev_src="$cand"; break; fi
    done
    if [ -n "$dev_src" ]; then
        echo "[fetch] extracting committed $dev_src (no network needed)..."
    else
        echo "[fetch] downloading kernel headers for the LKM ($DEV_APK, ~18 MB)..."
        curl -fL -o "$DEV_APK" "$BASE/$DEV_APK"
        dev_src="$DEV_APK"
    fi
    tar xzf "$dev_src" -C kern-dev 2>/dev/null || true
    echo "[fetch] headers ready: kern-dev/usr/src/linux-headers-6.6.142-0-virt"
fi

echo "[fetch] boot toolchain complete."
