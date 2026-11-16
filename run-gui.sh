#!/bin/bash
# run-gui.sh — boot AI-OS v1.1 straight into the graphical desktop.
# The aios-desktop shell owns the framebuffer; the serial console
# (-serial stdio) stays available for CI markers and the text shell.
cd "$(dirname "$0")"
exec qemu-system-x86_64 \
  -machine accel=tcg -m 768M -smp 2 \
  -kernel vmlinuz-virt \
  -initrd aios-initramfs.cpio.xz \
  -drive file=data.img,format=raw,if=virtio \
  -device bochs-display,xres=1280,yres=800 \
  -device virtio-keyboard-pci \
  -device virtio-tablet-pci \
  -append "console=ttyS0 rdinit=/init" \
  -serial stdio -no-reboot -nic none
