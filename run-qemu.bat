@echo off
rem run-qemu.bat - boot AI-OS m10 interactively (Windows)
rem Requires qemu-system-x86_64 on PATH (https://qemu.weilnetz.de/w64/)
rem data.img is the durable side disk (memory/jobs/audit/autopilot
rem survive reboots); delete it for a factory reset.
qemu-system-x86_64.exe ^
  -machine accel=tcg -m 512M -smp 2 ^
  -kernel vmlinuz-virt ^
  -initrd aios-initramfs.cpio.xz ^
  -drive file=data.img,format=raw,if=virtio ^
  -append "console=ttyS0 rdinit=/init" ^
  -nographic -no-reboot -nic none
