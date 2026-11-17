# AI-OS — an AI-native operating system on Linux

AI-OS treats AI as a **first-class citizen of the operating system**, not as
an application layered on top. The Linux kernel remains the base; the system
services layer above it is rewritten around three native abstractions:

- **aiproc** — an AI-native process. A process is an *intent* that the OS
  plans, schedules, executes and reflects on; it has semantic state, not
  just a PID.
- **sembus** — semantic IPC. Processes exchange typed, structured messages
  on topics; subscription is by meaning, not just by channel ID.
- **agentd** — the agent runtime (M4). Long-lived agents observe, plan,
  act through sandboxed tools, and reflect results into a persistent,
  3-tier memory owned by the OS itself.
- **aiosh** — the AI shell (M5). The command front-end: one line is a
  pipeline of tools, agents and builtins; anything the shell cannot
  parse syntactically is routed by meaning — the OS decides whether you
  meant one tool call, multi-step agent work, or a plain answer — and
  (M7) in WHICH scheduling class the work belongs.
- **autopilot** — the closed-loop AI scheduler (M7). An OS daemon reads
  what the kernel sees (per-class wakeups, context switches, CPU usage)
  and re-tunes the cgroup CPU budgets live: hungry classes borrow from
  quiet ones, with hysteresis and a journalled, human-readable reason.
- **micro-LM** — the OS's own very small language model (M7): trained at
  build time, ~12 KB of weights, classifies requests into scheduling
  classes and generates OS status prose. Deterministic, offline, and
  swappable for big LLMs at the cortex seam.
- **durable state** — an ext2 side disk (M8) makes the OS's memory
  real: memory journals, the audit log, job history and the autopilot
  journals live under `/data/aios` with crash-consistent, fsync'd
  writes — they survive reboots and power loss, and the next boot
  replays the previous session's journals before you log in.

```
+--------------------------------------------------------------+
|  aiosh  (M5)          AI shell: pipelines, jobs, routing      |
+--------------------------------------------------------------+
|  agentd (M4)          agent runtime: loop + cortex + tools   |
|     |                        |                    |           |
|  memory (3-tier,      tool registry        sandboxed         |
|  persistent,          (calc, fs.*,         tool-exec worker  |
|  embedded recall)      memory, proc)       (rlimit+timeout)  |
+--------------------------------------------------------------+
|  sembus (M3)          semantic IPC, typed topics             |
+--------------------------------------------------------------+
|  aiproc (M2)          intent-driven AI processes             |
+--------------------------------------------------------------+
|  core (M1)            types, json, status, versioning        |
+--------------------------------------------------------------+
|  Linux kernel         base: mm, sched, net, fs drivers       |
+--------------------------------------------------------------+
```

**Licence:** Apache-2.0 (see `LICENSE`). **Contributing:** see
`CONTRIBUTING.md` and `context/RULES.md`. **Roadmap to desktop capability:**
`context/ROADMAP-TO-WINDOWS.md`.

This repository is **AI-OS v1.1 (M10 `desktop`)** — a self-contained
C11 runtime layer (zero external dependencies, builds in seconds)
**plus a bootable OS image** — and since M10 the OS has a graphical
face: the boot lands in the **aios-desktop** framebuffer shell, whose
launcher talks to the same aiosh engine (and the same micro-LM) as the
serial console.

```
aios/
├── include/aios/     public headers (types, json, aiproc, sembus,
│                     mem, tools, agent, shell, ksched, ebpf,
│                     hardening, microlm, autopilot, durable,
│                     fb, win, input)
├── src/              implementations + intent router + tool-exec worker
│                     + M10 graphics: fb.c, win.c, input.c
├── apps/             aiosh (the shell), aios-governor (autopilot),
│                     aios-desktop (the graphical shell), aios-demo,
│                     aios-memctl
├── tests/            unit + integration tests (make test)
├── tools/            microlm_gen.py — build-time micro-LM trainer
│                     gen_font.py, img2aiosimg.py — M10 asset pipeline
│                     mkfs_ext2.py — deterministic ext2 data-disk builder
├── desktop/assets/   artist artwork contract (ASSETS.md); PNGs dropped
│                     here become .aiosimg at build, every asset also
│                     has a compiled-in procedural fallback
├── boot/             bootable image: aios-init (PID 1), mkcpio,
│                     LKM, GPU module stack, QEMU run/test harnesses
│                     (boot-test.sh, boot-gui-test.sh)
├── examples/         demo.aiosh — scripted shell session
├── workspace/        sandbox jail + sample files used by the demo
├── docs/             HARDENING.md, PERFORMANCE.md, ACCEPTANCE-v1.0.md,
│                     ACCEPTANCE-M10.md
├── INSTALL.md        install path (make install / tools/install.sh)
├── UPGRADE.md        upgrade path (binaries stateless, store survives)
├── CHANGELOG.md      release notes, M1..M10
└── ROADMAP.md        M1..M10 milestones — all DONE (v1.1 desktop)
```

## Build & test

```bash
make          # builds libaios.a, tool-exec, aiosh, aios-governor,
              # aios-demo, aios-memctl
make test     # builds and runs all tests (12 suites)
make install  # PREFIX=/usr/local (DESTDIR-aware); see INSTALL.md
```

## Boot it (M5.5 — a real OS)

M5.5 turns AI-OS into something you can boot: the Linux kernel comes up,
and PID 1 is `aios-init` — our own init — which runs the M1-M5 selftest
suite **inside the booted OS**, brings the AI shell up as the console
front-end, and drops you into `aiosh`.

```bash
make -f boot/Makefile boot-image    # build vmlinuz + aios-initramfs.cpio.xz
make -f boot/Makefile boot-run      # boot interactively (QEMU, serial console)
make -f boot/Makefile boot-test     # headless CI boot test: two boots on the
                                    #   same ext2 data disk — asserts the
                                    #   selftest AND the reboot replay
bash boot/boot-gui-test.sh          # the M10 proof: boots the DESKTOP with a
                                    #   GPU + keyboard, screendumps + pixel
                                    #   asserts, types 6*7 -> 42 in the GUI
bash boot/console-test.sh           # drive the live aiosh console over serial
```

On the serial console you are talking to the AI shell of the OS — and
with a display attached the boot lands directly in the graphical
desktop (see the M10 section below):

```
AI-OS 1.1.0-m10 (v1.1 desktop)
Linux 6.6.142-0-virt base | system services layer: AI-native ...
[init] durable: insmod virtio_blk.ko ok
[init] durable: insmod mbcache.ko ok
[init] durable: insmod ext2.ko ok
[init] durable: /dev/vda on /data (ext2) — restored 2 files,
       previous session: jobs=1 mem=9 audit=6 autopilot=1
[init] durable: this is boot #2 for this data disk
[init] autopilot governor: pid 433 (mode balanced)
[init] phase 1/2: AI-OS selftest (M1-M8 unit + integration suite)
  ... 12 suites ...
AIOS-SELFTEST: PASS
AIOS-BOOT-OK selftest=PASS

AI-OS console ready. You are talking to aiosh (the AI shell).
aiosh> 6*7
42
aiosh> dur
durable: /data/aios (ext2 side disk, 2 boots) — this shell's store is on it
  memory:     18 entries
  audit:      12 tool calls
  jobs:       2 spawned / 4 journal records
  autopilot:  2 journal entries (last: {"tick":1,...})
aiosh> lm classify run it later in the background
lm: class=bg conf=0.95 (micro-lm) text="run it later in the background"
aiosh> autopilot
autopilot: mode=balanced governor-ticks=14 adjustments=0 registry=57
  live budgets: tools=50% agents=25% (cgroup cpu.max)
aiosh> recall AI-OS
[0.47] episodic AI-OS m8 durable boot completed  <- boot memory, OS-owned,
                                                 and it survived the reboot
```

Boot artifacts do not require root to build (custom no-root cpio packer
with device-node support) and require only `qemu-system-x86_64` to run.

## Kernel bridge (M6)

Above the boot floor, the OS now couples to the kernel it runs on:

- **`aios_intent.ko`** — our loadable kernel module: `/dev/aios` intent
  registry (aiosh = INTERACTIVE, tool workers = BATCH, background jobs =
  IDLE) and `/proc/aios` with per-class wakeup counts from a kprobe.
- **eBPF wakeup observer** — a 21-instruction program, hand-assembled in
  `src/ebpf.c` (no libbpf, no clang), attached to `sched:sched_wakeup`.
- **cgroup v2 + sched_setattr** — AI work gets kernel-enforced budgets:
  tools ≤ 50% CPU, background agents ≤ 25%, the console never throttled.
- **seccomp + Landlock** — the tool worker's syscalls are allowlisted and
  its filesystem view locked to the jail by the kernel itself.

Inspect any of it live from the console with the `kernel` builtin.

## Durable state (M8)

The OS now remembers. An 8 MiB ext2 side disk (`data.img`, attached by
`run-qemu.sh` automatically) is mounted by init as `/data`, and everything
the OS owns lives there in a crash-consistent store:

```
/data/aios/
├── store/memory.journal    3-tier memory (episodic + semantic), fsync'd
├── store/audit.log         every tool call, append-only
├── store/jobs.jsonl        job spawn/exit records — job history survives
├── store/jobs/<id>.json    per-job result documents
├── var/                    mirrors of /var/lib/aios (autopilot conf/state)
├── autopilot/              governor write-through state + journal
└── boots.jsonl             one line per boot — the OS counts its own boots
```

Writes are atomic (temp + fsync + rename + directory fsync) or append+fsync;
the replay reader treats a torn last line as a crash artifact and drops it.
At boot, init loads the storage stack (`virtio_blk` + `mbcache` + `ext2`),
mounts the disk, restores the previous session's state, and prints what it
found — the two-boot CI test (`boot-test.sh`) fails unless a second boot
actually sees the first boot's state. `aios.durable=0` returns the OS to
m7 behavior; delete `data.img` for a factory reset.

## Demo

```bash
./aiosh                                  # the AI shell (scripted cortex)
./aiosh --llm                            # shell with LLM intent routing
./aiosh examples/demo.aiosh              # scripted session, deterministic
./aios-demo --scripted                   # M4 agent demo, offline cortex
./aios-memctl .aios-store recall "token estimate"   # inspect OS memory
```

Run the demo twice in a row: the second run starts with a memory that
already contains the first run's facts — the agent recalls them instead of
recomputing. That is the OS owning memory, not the app.

Verified transcript (real LLM cortex, goal = "summarize this workspace ..."):

```
  [sembus] agent.start: {"agent":"architect","cortex":"llm"}
  [sembus] agent.done:  {"agent":"architect","status":"achieved"}

== agent finished in 6244 ms (goal achieved) ==
Workspace summary complete with token estimates

memory: working=1 episodic=8 semantic=3 journal=4340B
```

The model's plan, as recorded in the OS audit log (`.aios-store/audit.log`):

```
fs.list  | ok: true | {"path":"."}
fs.read  | ok: true | {"path":"notes.txt"}
fs.read  | ok: true | {"path":"metrics.csv"}
calc     | ok: true | {"expr":"352+101"}
fs.write | ok: true | {"path":"summary.md","content":"notes.txt: ..."}
fs.read  | ok: true | {"path":"summary.md"}   <- the agent verified its own output
```

and the file it wrote (jail: `./workspace/summary.md`):

```
notes.txt: AI-OS design notes (sample workspace file) - 88 tokens
metrics.csv: CSV file with module metrics - 25 tokens

Total token estimate: 113 tokens
```

## M5 — the shell, verified transcript

`./aiosh examples/demo.aiosh` (scripted cortex, deterministic — the same
session works with `--llm` for real natural-language routing):

```
$ aiosh examples/demo.aiosh
version                     -> AI-OS 0.5.0-m5 — aiosh (M5)
set f notes.txt             -> f=notes.txt
echo file is $f             -> file is notes.txt
fs.read $f                  -> AI-OS design notes (sample workspace file)...
fs.read $f | count          -> 7                 <- file CONTENT flows down the pipe
echo 12*(7+5) | calc        -> 144               <- piped expression evaluated
12*(7+5)                    -> 144               <- natural language: routed to calc
read notes.txt              -> (file content)    <- routed to fs.read
list files                  -> summary.md / notes.txt / metrics.csv
what can you do             -> (scripted cortex) conversational answers need
                               the llm cortex... <- routed to reply
summarize the workspace     -> Wrote summary.md: 3 files, 544 bytes, ~136 tokens.
remember aiosh demo ...     -> remembered: aiosh demo completed a full pipeline
recall aiosh demo           -> [0.53] episodic aiosh demo completed a full pipeline
emit demo.done '{"ok":true}' -> published to demo.done (0 subscribers)
echo final status $?        -> final status 0
```

and a background agent job, supervised like a process:

```
aiosh> spawn summarize the workspace
[job 1] spawned: summarize the workspace
aiosh> jobs
  1  running   summarize the workspace
aiosh> wait 1
[job 1] done: Wrote summary.md: 3 files, 544 bytes, ~136 tokens.
```

The spawned job runs as a separate OS process with its **own** memory
context, tool registry and cortex; its result lands in
`<store>/jobs/1.json` and every line, agent run and spawn publishes
events (`sh.cmd`, `sh.agent.start/done`, `sh.spawn`) on the shell's
semantic bus.

## M4 highlights

| Component | What it does |
|-----------|--------------|
| `mem`     | 3 tiers: working (RAM ring), episodic (journal, capped), semantic (consolidated facts). 256-dim hashed char-n-gram embeddings, cosine relevance, salience = 0.62*relevance + 0.23*recency + 0.15*use. Append-only JSONL journal with fsync, atomic compaction. |
| `tools`   | Registry with native tools (`memory.search`, `proc.info`) and sandboxed tools (`calc`, `fs.read`, `fs.write`, `fs.list`) executed in a separate `tool-exec` worker process. |
| sandbox   | fork/exec worker with rlimits (CPU, AS 256MB, NOFILE, NPROC, FSIZE), wall-clock timeout enforced twice (child alarm + parent waitpid deadline with SIGKILL), cwd jailed, path traversal rejected, `PR_SET_NO_NEW_PRIVS`. |
| `agent`   | observe -> plan -> act -> reflect loop, max-steps budget, reflection writes back to memory (episodic trail + durable semantic facts); cortex is pluggable (`llm` or `scripted`). The **runtime enforces a no-duplicate-call rule**: an identical (tool,args) call in the same session is rejected with an explanatory error — loop discipline is an OS service, not a model virtue. |
| `llm bridge` | `z-ai chat` subprocess per step, strict-JSON action contract with one corrective retry, 429-aware backoff (10s/20s/40s), child stdio isolated, CLI diagnostics surfaced on failure. |
| audit     | Every tool call appended as JSONL to `.aios-store/audit.log` (who/when/args/ok/duration/result digest). |

## Tests

```
$ make test
PASS build/tests/test_json         (20 checks)
PASS build/tests/test_aiproc       ( 9 checks)
PASS build/tests/test_sembus       (15 checks)
PASS build/tests/test_mem          (25 checks)
PASS build/tests/test_sandbox      (26 checks)
PASS build/tests/test_agent        (15 checks)
PASS build/tests/test_shell        (100 checks)
PASS build/tests/test_kernel       ( 4 checks, 5 env-skipped on the host;
                                    full pass inside the booted OS)
PASS build/tests/test_autopilot    (61 checks, 3 env-skipped)
PASS build/tests/test_durable      (35 checks, 1 env-skipped)
PASS build/tests/test_fb           (89 checks)
== 12 passed, 0 failed ==
```

The sandbox suite proves: arithmetic evaluation, fs round-trips inside the
jail, rejection of `..`/absolute path escapes, timeout kill of a wedged
worker (parent-enforced SIGKILL), unknown-tool handling, and audit-log
integrity. The agent suite runs a full two-session agentic loop with the
scripted cortex and asserts memory writeback and cross-session recall.
The shell suite covers the parser (quoting, `key=value`, `{json}`, pipes,
sequences), variable expansion (`$var $out $? $json $$`), text builtins,
tool stages with pipe injection, the scripted intent router (unit),
foreground agent runs, background jobs (spawn/jobs/wait/kill incl. kill
of a running agent), memory builtins, sembus events, audit integrity and
parse-error handling.

## The desktop (M10, v1.1)

After the selftest, PID 1 loads the GPU/input module stack (18 gzipped
kernel modules, gunzip + insmod at boot), draws a splash on
`/dev/fb0`, and then supervises TWO console front-ends forever:

- **aios-desktop** on the framebuffer: wallpaper, a launcher bar whose
  input box speaks natural language to aiosh (micro-LM cortex), and
  app windows — **terminal** (the aiosh REPL), **files** (a browser
  over `/workspace`), **agents** (background job list), **memory**
  (remember/recall), **autopilot** (live governor state + budget
  bars). Semibus events surface as toasts.
- **aiosh** on serial: the M1-M9 console, unchanged (CI uses it).

Flags: `aios.gui=0` skips the GPU stack (pure text boot); five
consecutive desktop crashes fall back to the text console. The
graphics path is evidence-picked: `bochs-display` (plain PCI-BAR
framebuffer — guest writes are visible immediately) instead of
virtio-gpu (whose scanout needs a display backend). Every artwork
asset has a compiled-in procedural fallback, so the desktop boots with
a full face even with zero artist PNGs (`desktop/assets/ASSETS.md` is
the drop-in contract for real artwork). Proof: `boot/boot-gui-test.sh`
screendumps the desktop through QEMU, pixel-asserts the wallpaper /
accent bar / five title bars / cursor, then types `6*7` through the
virtio keyboard — the GUI answers `42` (screenshots in
the script's own `download/m10-boot-shots/`). Acceptance record:
[docs/ACCEPTANCE-M10.md](docs/ACCEPTANCE-M10.md).

## Install, upgrade, hardening, performance (M9 / v1.0)

- **Install**: `make install` (`PREFIX`/`DESTDIR` aware) or
  `./tools/install.sh` (build -> 10-suite test -> install -> verify).
  See [INSTALL.md](INSTALL.md).
- **Upgrade**: binaries are stateless; the OS-owned store survives
  replacement. See [UPGRADE.md](UPGRADE.md).
- **Hardening review**: threat model, sandbox layer map, compiler
  hardening (v1.0 builds warning-free with `-fstack-protector-strong`,
  `-D_FORTIFY_SOURCE=2`, `-Wformat-security`), and the
  claim->enforcement->test matrix. See [docs/HARDENING.md](docs/HARDENING.md).
- **Performance**: measured numbers — 1.9 s clean build, 3.35 s full
  test battery, ~2 ms per interactive shell line including a sandboxed
  tool call, ~1 ms per micro-LM classify, 1.5 ms memory recall on a
  100-entry store. See [docs/PERFORMANCE.md](docs/PERFORMANCE.md).
- **Acceptance**: the full v1.0 acceptance record (host suites, two-boot
  durable CI proof, console test, size guard) lives in
  [docs/ACCEPTANCE-v1.0.md](docs/ACCEPTANCE-v1.0.md).

## Security model (honest scope)

The M4 sandbox is a *practical* jail: separate process, resource limits,
timeouts, path jailing. It is not yet a kernel-enforced security boundary.
M6 adds seccomp-BPF filter sets and Landlock LSM rules inside `tool-exec`,
plus an eBPF scheduler-hint path. This is the same staging used for any
OS: capability first, hardening second.

Shell scope notes (M5): line editing is plain `fgets` (no readline
dependency); backslash escapes are not supported — use single quotes for
literal JSON (`calc '{"expr":"2+3"}'`); spawned jobs share the memory
store with the shell (JSONL appends are line-atomic) but consolidation
stays a foreground operation.

## Layout of the runtime API

```c
aios_mem *m = mem_open(".aios-store");
mem_remember(m, MEM_EPISODIC, "built token estimate for workspace", NULL);
mem_entry *hits;  int n = mem_recall(m, "how many tokens?", 3, &hits);

tool_reg *tr = tool_reg_create("./workspace");     // jail dir
json_val *out;
tool_exec(tr, "calc", json_args("12*(7+5)"), &out);

cortex *cx = cortex_llm_create(NULL);              // or cortex_scripted()
agent_run(&(agent_cfg){.name="architect", .goal="summarize workspace",
                       .cx=cx, .tools=tr, .mem=m, .max_steps=8}, buf, sizeof buf);

sh_shell *sh = sh_create("./workspace", ".aios-store");   // M5: the shell
sh_set_cortex(sh, "llm");
sh_result r;
sh_exec(sh, "fs.read notes.txt | count", &r);      // pipes
sh_exec(sh, "summarize the workspace", &r);        // NL -> agent runtime
sh_exec(sh, "spawn summarize the workspace", &r);  // background job
sh_result_free(&r);
sh_free(sh);
```
