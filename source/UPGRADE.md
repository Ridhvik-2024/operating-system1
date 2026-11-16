# AI-OS — Upgrade Guide

## The one rule

**Binaries are stateless; the OS owns the state.** Everything the OS
remembers lives in the durable store, not in the binaries or config
files:

| installation | store location | what lives there |
|---|---|---|
| host runtime | `./.aios-store` (or `$AIOS_STORE`) | memory journals (episodic + semantic), audit log, job history |
| booted OS | `/data/aios` on the ext2 `data.img` side disk | all of the above **plus** autopilot conf/state/journal, boot count, mirrors of `/var/lib/aios` |

Upgrading = replacing binaries. The store format is append-only JSONL
and JSON documents and has not changed since M4 (host) / M8 (disk);
there is no migration step.

## Upgrade: host runtime (any version -> v1.0)

```bash
cd aios                      # the new source tree (or git pull)
./tools/install.sh           # builds, runs the 10-suite test, installs
aiosh -c version             # AI-OS 1.0.0 — aiosh (v1.0)
```

Your old memory is picked up automatically: point the new binaries at
the same store directory (default `./.aios-store` relative to where
you run aiosh, or `AIOS_STORE=/path/to/.aios-store`).

```bash
aiosh -c 'recall v1.0'       # proof: old facts still recallable
```

Rollback: `make uninstall` removes v1.0 binaries; install the previous
release the same way. The store is never touched by install/uninstall.

## Upgrade: booted OS (m8 -> v1.0)

```bash
# in the release bundle directory:
cp vmlinuz-virt  /vm/my-aios/vmlinuz-virt          # replace
cp aios-initramfs.cpio.xz /vm/my-aios/             # replace
# data.img stays EXACTLY where it is — do not delete it
./boot-test.sh                                     # verify (two boots)
./run-qemu.sh                                      # and boot
```

What you will see on the first v1.0 boot:

- banner changes from `AI-OS 0.8.0-m8 (m8 durable)` to
  `AI-OS 1.0.0 (v1.0)` — the boot number keeps counting
  (`this is boot #N for this data disk`)
- the previous session's jobs/memory/audit/autopilot journals replay
  exactly as under m8 (the durable store format is unchanged)
- `dur` reports the same store with the same counts

Rollback: put the m8 kernel/initramfs back. Same store, no migration
in either direction.

### Upgrading from m7 or earlier (RAM-only)

m6/m7 kept state in RAM (or in `./.aios-store` on the host); nothing
to migrate — boot v1.0 and the durable store starts fresh. On the
host, copy or point `AIOS_STORE` at your existing `.aios-store` and
the memory carries over.

## Factory reset

- host: delete the store directory (`rm -rf .aios-store`)
- booted OS: delete `data.img` (or re-run `python3 tools/mkfs_ext2.py
  data.img` to reformat it deterministically)

## Version check list (post-upgrade)

1. `aiosh -c version` → `AI-OS 1.0.0 — aiosh (v1.0)`
2. booted OS banner → `AI-OS 1.0.0 (v1.0)`
3. `dur` → boot count + store counts preserved
4. `kernel` → registry, cgroup budgets, both eBPF observers
5. `autopilot` → governor alive, mode from `tune` conf (persisted)
6. `make test` on the host → `== 10 passed, 0 failed ==`
