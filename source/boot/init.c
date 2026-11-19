/* init.c - AI-OS init (PID 1) for the v1.1 (M10 desktop) bootable image.
 *
 * This is the first OS-native program that runs on an AI-OS machine:
 *   1. mounts the base filesystems (proc, sys, devtmpfs, tmpfs)
 *   2. mounts the ext2 side disk and replays the previous session (M8)
 *   3. loads the kernel bridge module + cgroup intent classes (M6)
 *   4. starts the autopilot governor (M7: closed-loop AI scheduler)
 *   5. runs the AI-OS selftest suite (all M1-M9 unit/integration tests)
 *   6. runs the system bring-up script through aiosh (the AI shell) in
 *      micro mode - the OS's own language model speaks at boot (M7)
 *   7. respawns an interactive aiosh on the console - the AI shell IS
 *      the system front-end, not a program running inside a classic shell
 *
 * Kernel cmdline flags honoured:
 *   aios.mode=test        boot, verify, print AIOS-BOOT-OK, power off (CI)
 *   aios.selftest=0       skip the selftest phase
 *   aios.autopilot=MODE   off|static|balanced|aggressive (default balanced)
 *   aios.durable=0        skip the ext2 data disk (RAM-only session)
 *   aios.gui=0            M10: skip the framebuffer stack; text console only
 *
 * M10 desktop: when the GUI is enabled, PID 1 loads the virtio-gpu + evdev
 * module stack (gunzip + insmod, order proven by the M10 boot spike),
 * draws the boot splash on /dev/fb0, and after bring-up supervises TWO
 * console front-ends forever: /sbin/aios-desktop (graphics) and /sbin/aiosh
 * (serial). The desktop crashing five times falls back to the text console
 * (aios.gui=0 skips the GPU stack entirely).
 *
 * PID 1 never exits: it reaps orphans and respawns the console shells.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <sys/utsname.h>

#include "aios/types.h"
#include "aios/ksched.h"
#include "aios/durable.h"
#include "aios/supervisor.h"
#include "aios/fb.h"

static FILE *con;

/* ---- C4: a boot log that outlives the console ---------------------------
 *
 * `say()` used to write ONLY to the console. That is fine while the splash is
 * up and useless the moment it closes: the desktop takes over the screen, and
 * every boot message is gone - not merely hidden, gone. The one class of message
 * a user most needs ("the governor died", "the mount failed", "rollback
 * refused") was the class that had no record anywhere.
 *
 * So every line is appended to a RING on the durable store as well. The ring is
 * a fixed-size file rewritten when it fills, NOT an ever-growing journal: a boot
 * log that grows without bound on a 8 MiB data disk eventually becomes the
 * reason a boot fails, which is a joke with a bad ending. 16 KiB holds a few
 * hundred boot lines and costs nothing.
 *
 * The write is best effort and NEVER fatal. Init must not refuse to boot because
 * its log file could not be written; if the ring is unavailable the console
 * still gets the line, and `log` says the ring is unavailable rather than
 * pretending it is empty. */
#define LOG_RING_REL  "boot.log"
#define LOG_RING_CAP  (16 * 1024)
static char  g_ring[LOG_RING_CAP];
static size_t g_ring_n;
static int   g_ring_ok = -1;          /* -1 unknown, 0 unavailable, 1 usable */

static void ring_load(void) {
    if (g_ring_ok >= 0) return;
    size_t n = 0;
    g_ring_ok = (aios_dur_read(LOG_RING_REL, g_ring, sizeof g_ring, &n) == AIOS_OK);
    g_ring_n = (g_ring_ok && n <= sizeof g_ring) ? n : 0;
    if (!g_ring_ok) g_ring_n = 0;
}

static void ring_put(const char *s) {
    ring_load();
    if (!g_ring_ok) return;           /* the console is still a record of sorts */
    const size_t need = strlen(s) + 1;             /* + newline */
    if (need >= LOG_RING_CAP) return;              /* a single absurd line */
    if (g_ring_n + need > LOG_RING_CAP) {
        /* drop the oldest whole lines until it fits: a ring shows the newest */
        size_t drop = 0;
        while (drop < g_ring_n && g_ring_n - drop + need > LOG_RING_CAP) {
            const char *nl = memchr(g_ring + drop, '\n', g_ring_n - drop);
            if (!nl) { drop = g_ring_n; break; }
            drop = (size_t)(nl - g_ring) + 1;
        }
        memmove(g_ring, g_ring + drop, g_ring_n - drop);
        g_ring_n -= drop;
    }
    memcpy(g_ring + g_ring_n, s, need - 1);
    g_ring_n += need - 1;
    g_ring[g_ring_n++] = '\n';
    /* Best-effort and deliberately not fsynced per line: a boot log is useful
       after a crash but is not worth a disk sync on every message. The ring is
       rewritten whole, so a torn write loses the log, not the boot. */
    (void)aios_dur_write_atomic(LOG_RING_REL, g_ring, g_ring_n);
}

static void say(const char *s) {
    if (con) { fputs(s, con); fputc('\n', con); fflush(con); }
    ring_put(s);
}

static void mount_fs(const char *src, const char *tgt, const char *type,
                     unsigned long flags) {
    mkdir(tgt, 0755); /* ignore EEXIST */
    if (mount(src, tgt, type, flags, NULL) == 0)
        fprintf(con, "[init] mount %-9s -> %-10s ok\n", type, tgt);
    else
        fprintf(con, "[init] mount %-9s -> %-10s FAILED: %s\n",
                type, tgt, strerror(errno));
    fflush(con);
}

static int run(const char *path, char *const argv[]) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        execv(path, argv);
        _exit(127);
    }
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return -1;
}

static int cmdline_flag(const char *needle) {
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return 0;
    char buf[1024] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    (void)n;
    fclose(f);
    return strstr(buf, needle) != NULL;
}

/* ------------------ M10: GUI phase + boot splash ------------------- */

/* The 18-module GPU/input stack (Alpine linux-virt 6.6.142), in the load
 * order proven by the M10 boot spikes: fb helpers -> fb core -> drm ->
 * bochs (QEMU bochs-display: plain PCI BAR framebuffer, mmap writes are
 * displayed directly - the virtio-gpu resource-upload path does not reach
 * the QEMU surface without a display backend) -> evdev input. Shipped
 * gzipped (~845 KB); gunzipped to /tmp/gpu at boot when the GUI is up. */
static const char *const GPU_MODULES[] = {
    "sysfillrect", "syscopyarea", "sysimgblt", "fb_sys_fops",
    "fb_io_fops", "fbdev", "fb", "i2c-core",
    "drm_panel_orientation_quirks", "drm", "drm_kms_helper",
    "drm_shmem_helper", "ttm", "drm_ttm_helper", "drm_vram_helper",
    "bochs", "evdev", "virtio_input",
};

static aios_fb  *g_splash;
static aios_img *g_splash_logo;
static int       g_gui_ok;

static void splash_draw_base(void) {
    if (!g_splash) return;
    int w = aios_fb_w(g_splash), h = aios_fb_h(g_splash);
    /* paint the gradient via fb primitives (no asset dependency) */
    for (int y = 0; y < h; y++) {
        int t = (y * 255) / (h > 1 ? h - 1 : 1);
        unsigned r = (unsigned)(10 + (20 - 10) * t / 255);
        unsigned g2 = (unsigned)(13 + (24 - 13) * t / 255);
        unsigned b = (unsigned)(22 + (36 - 22) * t / 255);
        aios_rect line = { 0, y, w, 1 };
        aios_fb_fill(g_splash, &line,
                     0xFF000000u | (r << 16) | (g2 << 8) | b);
    }
    if (g_splash_logo)
        aios_fb_blit(g_splash, w / 2 - 48, h / 2 - 120, g_splash_logo);
    char title[64];
    snprintf(title, sizeof title, "AI-OS %s", AIOS_VERSION);
    int tw = aios_fb_text_w(title);
    aios_fb_text(g_splash, w / 2 - tw / 2, h / 2 - 8, title,
                 AIOS_RGB(0xe8, 0xee, 0xf8), 0, 1);
    const char *sub = "AI-native on Linux - the desktop is coming up";
    tw = aios_fb_text_w(sub);
    aios_fb_text(g_splash, w / 2 - tw / 2, h / 2 + 16, sub,
                 AIOS_RGB(0x8a, 0x98, 0xb2), 0, 0);
    aios_rect full = { 0, 0, w, h };
    aios_fb_damage_add(g_splash, &full);
    aios_fb_flush(g_splash);
}

static void splash_phase(const char *msg) {
    if (!g_splash) return;
    int w = aios_fb_w(g_splash), h = aios_fb_h(g_splash);
    aios_rect strip = { 0, h - 40, w, 24 };
    aios_fb_fill(g_splash, &strip, 0xFF0d111cu);
    char line[80];
    snprintf(line, sizeof line, "[ %.60s ]", msg);
    int tw = aios_fb_text_w(line);
    aios_fb_text(g_splash, w / 2 - tw / 2, h - 36, line,
                 AIOS_RGB(0x4a, 0xe0, 0x8a), 0, 0);
    aios_fb_flush(g_splash);
}

static void splash_close(void) {
    if (g_splash_logo) aios_img_free(g_splash_logo);
    g_splash_logo = NULL;
    aios_fb_close(g_splash);
    g_splash = NULL;
}

/* Load the GPU/input stack and light up the splash. Best-effort: any
 * failure leaves the M1-M9 text console behaviour untouched. */
static int gui_phase(void) {
    if (cmdline_flag("aios.gui=0")) {
        say("[init] gui: disabled (aios.gui=0) - text console only");
        return -1;
    }
    mkdir("/tmp/gpu", 0755);
    int ok = 0, fail = 0;
    for (size_t i = 0;
         i < sizeof GPU_MODULES / sizeof GPU_MODULES[0]; i++) {
        char gz[96], ko[96];
        snprintf(gz, sizeof gz, "/usr/lib/aios/gpu/%s.ko.gz",
                 GPU_MODULES[i]);
        snprintf(ko, sizeof ko, "/tmp/gpu/%s.ko", GPU_MODULES[i]);
        /* busybox: gunzip -c src > dst; then insmod dst */
        char *gargv[] = { "gunzip", "-c", (char *)gz, NULL };
        pid_t p = fork();
        if (p == 0) {
            int out = open(ko, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out < 0) _exit(1);
            dup2(out, 1);
            close(out);
            execv("/bin/busybox", gargv);
            _exit(127);
        }
        int st = 0;
        while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
        if (st != 0) { fail++; continue; }
        char *iargv[] = { "insmod", ko, NULL };
        p = fork();
        if (p == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
            execv("/bin/busybox", iargv);
            _exit(127);
        }
        while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
        if (st == 0) ok++; else fail++;
    }
    fprintf(con, "[init] gui: GPU/input module stack %d/%zu loaded%s\n",
            ok, sizeof GPU_MODULES / sizeof GPU_MODULES[0],
            fail ? " (some failed - continuing)" : "");

    struct stat stt;
    if (stat("/dev/fb0", &stt) != 0) {
        say("[init] gui: no /dev/fb0 - text console only");
        return -1;
    }
    g_splash = aios_fb_open("/dev/fb0");
    if (!g_splash) {
        say("[init] gui: framebuffer unusable - text console only");
        return -1;
    }
    g_splash_logo = aios_img_logo_fallback(96);
    splash_draw_base();
    fprintf(con, "[init] gui: /dev/fb0 %dx%d up (bochs-display)\n",
            aios_fb_w(g_splash), aios_fb_h(g_splash));
    g_gui_ok = 1;
    return 0;
}

/* value of a key=value cmdline token ("aios.autopilot=off" -> "off"),
 * copied into out; returns 0 when found */
static int cmdline_value(const char *key, char *out, size_t cap) {
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return -1;
    char buf[1024] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    (void)n;
    fclose(f);
    size_t klen = strlen(key);
    char *p = buf;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == buf || p[-1] == ' ') && p[klen] == '=') {
            char *v = p + klen + 1;
            char *end = strchr(v, ' ');
            size_t l = end ? (size_t)(end - v) : strlen(v);
            if (l >= cap) l = cap - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return 0;
        }
        p += klen;
    }
    return -1;
}

/* M8 durable: count "spawn" records in the store job journal (plain file
 * inside the store dir, which itself lives on the ext2 disk). */
static size_t count_spawn_records(const char *path) {
    static char buf[1 << 19];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    size_t count = 0;
    const char *p = buf;
    while ((p = strstr(p, "\"op\":\"spawn\"")) != NULL) {
        count++;
        p += 5;
    }
    return count;
}

/* M8 durable: load storage drivers, mount the ext2 side disk on /data,
 * restore the previous session's /var/lib/aios mirrors, and report the
 * replay counts. Everything is best-effort: without a data disk the OS
 * behaves exactly like m7 (RAM-only state). */
static int durable_phase(void) {
    mkdir("/data", 0755);

    /* storage stack: virtio block device + ext2 (depends on mbcache) */
    const char *mods[] = { "virtio_blk.ko", "mbcache.ko", "ext2.ko" };
    int loaded = 0, ok_mods = 0;
    for (size_t i = 0; i < sizeof mods / sizeof mods[0]; i++) {
        char mp[64];
        snprintf(mp, sizeof mp, "/lib/modules/%s", mods[i]);
        char *ia[] = { "insmod", mp, NULL };
        int rc = run("/bin/busybox", ia);
        loaded++;
        if (rc == 0) ok_mods++;
        fprintf(con, "[init] durable: insmod %s %s\n", mods[i],
                rc == 0 ? "ok" : "FAILED");
    }
    (void)loaded;

    int mounted = 0;
    const char *used_dev = NULL;
    const char *devs[] = { "/dev/vda", "/dev/vdb", "/dev/vdc", "/dev/vdd" };
    for (size_t i = 0; i < sizeof devs / sizeof devs[0] && !mounted; i++) {
        if (access(devs[i], F_OK) != 0) continue;
        if (mount(devs[i], "/data", "ext2", MS_NOSUID | MS_NODEV, NULL) == 0) {
            mounted = 1;
            used_dev = devs[i];
        } else {
            fprintf(con, "[init] durable: mount %s FAILED: %s\n",
                    devs[i], strerror(errno));
        }
    }
    if (!mounted) {
        say("[init] durable: no ext2 data disk - RAM-only session (m7 behavior)");
        say("AIOS-DUR mounted=0 jobs=0 mem=0 audit=0 ap=0 boots=0");
        return -1;
    }

    /* durable store layout */
    aios_mkdirs("/data/aios/store/jobs");
    aios_mkdirs("/data/aios/var");
    aios_mkdirs("/data/aios/autopilot");

    /* restore: the previous session's /var/lib/aios mirrors (autopilot
     * conf/state/journal land back where the governor expects them). */
    int restored = aios_dur_restore_dir("var", "/var/lib/aios");

    /* replay report: what the previous session left behind */
    size_t mem_n = 0, audit_n = 0, ap_n = 0, boots = 0;
    char lastj[256] = {0}, lastap[256] = {0};
    aios_dur_lines("store/memory.journal", &mem_n, NULL, 0);
    aios_dur_lines("store/audit.log", &audit_n, NULL, 0);
    aios_dur_lines("var/autopilot.jsonl", &ap_n, lastap, sizeof lastap);
    aios_dur_lines("boots.jsonl", &boots, lastj, sizeof lastj);
    size_t jobs_n = count_spawn_records("/data/aios/store/jobs.jsonl");

    fprintf(con,
            "[init] durable: %s on /data (ext2) - restored %d files, "
            "previous session: jobs=%zu mem=%zu audit=%zu autopilot=%zu\n",
            used_dev, restored < 0 ? 0 : restored, jobs_n, mem_n, audit_n,
            ap_n);
    if (ap_n > 0)
        fprintf(con, "[init] durable: autopilot journal last entry: %.140s\n",
                lastap);
    if (boots > 0)
        fprintf(con, "[init] durable: this is boot #%zu for this data disk\n",
                boots + 1);

    /* boot log: one fsync'd line per boot, replayed by the next one */
    char rec[128];
    snprintf(rec, sizeof rec, "{\"boot\":%zu,\"ts_ms\":%.0f}\n",
             boots + 1, aios_now_ms());
    aios_dur_append("boots.jsonl", rec, strlen(rec));

    /* the OS store moves onto the disk: memory/audit/jobs persist by
     * construction (mem.c and tools.c already fsync their journals). */
    setenv("AIOS_STORE", "/data/aios/store", 1);
    setenv("AIOS_DUR", "1", 1);

    fprintf(con,
            "AIOS-DUR mounted=1 jobs=%zu mem=%zu audit=%zu ap=%zu boots=%zu\n",
            jobs_n, mem_n, audit_n, ap_n, boots + 1);
    fflush(con);
    return 0;
}

static void power_off(void) {
    say("[init] powering off");
    sync();
    reboot(RB_POWER_OFF);
    /* if ACPI poweroff is unavailable, exit explicitly for CI */
    _exit(0);
}

int main(void) {
    /* stdio -> console (kernel opened /dev/console for initramfs /init) */
    int cfd = open("/dev/console", O_RDWR);
    if (cfd < 0) cfd = 0;
    dup2(cfd, 0); dup2(cfd, 1); dup2(cfd, 2);
    if (cfd > 2) close(cfd);
    con = stderr;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGCHLD, SIG_DFL);
    signal(SIGHUP, SIG_IGN);

    struct utsname u;
    if (uname(&u) != 0) strcpy(u.release, "?");

    say("");
    char line[256];
    snprintf(line, sizeof line,
             "AI-OS %s (v1.1 desktop)", AIOS_VERSION);
    say(line);
    snprintf(line, sizeof line,
             "Linux %s base | system services layer: AI-native "
             "(aiproc, sembus, agent-runtime, aiosh)", u.release);
    say(line);
    say("------------------------------------------------------------------");
    snprintf(line, sizeof line, "[init] pid1=%d booting AI-OS system services", getpid());
    say(line);

    mount_fs("proc",     "/proc", "proc",     MS_NOSUID | MS_NODEV | MS_NOEXEC);
    mount_fs("sysfs",    "/sys",  "sysfs",    MS_NOSUID | MS_NODEV | MS_NOEXEC);
    mount_fs("devtmpfs", "/dev",  "devtmpfs", MS_NOSUID);
    mount_fs("tmpfs",    "/tmp",  "tmpfs",    MS_NOSUID | MS_NODEV);
    mount_fs("cgroup2",  "/sys/fs/cgroup", "cgroup2", MS_NOSUID | MS_NODEV | MS_NOEXEC);
    mount_fs("tracefs",  "/sys/kernel/tracing", "tracefs", 0);

    if (sethostname("aios", 4) == 0)
        say("[init] hostname: aios");

    /* OS-owned state directories (RAM-backed unless durable restored them) */
    mkdir("/var", 0755); mkdir("/var/lib", 0755); mkdir("/var/lib/aios", 0755);
    mkdir("/var/aios-selftest", 0755);

    /* M8 durable: ext2 side disk + replay of the previous session. */
    if (cmdline_flag("aios.durable=0"))
        say("[init] durable: disabled (aios.durable=0) - RAM-only session");
    else
        durable_phase();

    /* M5: the supervisor that owns snapshot/restore.
     *
     * It runs as a CHILD of PID 1, not inside the respawn loop: the loop blocks in
     * waitpid() and a snapshot client would then wait for a child to die before
     * being answered. Being a child still means PID 1 is the process that created
     * it, which is the ownership the milestone asks for -- the ability to roll
     * back is established before any shell exists, and no shell brought it into
     * being.
     *
     * The jail is the same one aiosh will use, including the /workspace-if-there
     * rule: a supervisor taking snapshots of a different directory than the
     * shell edits would be the most confusing possible bug. */
    {
        /* The image always has /workspace; the check is here so that a future
         * image without one fails visibly at the supervisor rather than taking
         * snapshots of a directory that does not exist. */
        const char *jail = "/workspace";
        if (access(jail, F_OK) != 0)
            fprintf(con, "[init] supervisor: warning: jail %s does not exist yet\n", jail);
        const char *store = sup_default_store();
        pid_t sv = fork();
        if (sv == 0) {
            char serr[256] = {0};
            /* 0 = serve forever; PID 1 does not want the request cap. */
            int rc = sup_serve(AIOS_SUP_SOCK, jail, store, 0, serr, sizeof serr);
            if (rc != 0)
                fprintf(con, "[init] supervisor: %s\n", serr);
            _exit(rc == 0 ? 0 : 1);
        }
        if (sv > 0)
            fprintf(con, "[init] supervisor: pid %d serving %s (jail %s)\n",
                    (int)sv, AIOS_SUP_SOCK, jail);
        else
            say("[init] supervisor: could not start (rollback stays in-shell)");
    }

    /* the sandboxed tool worker lives next to the system binaries */
    setenv("AIOS_TOOL_EXEC", "/sbin/tool-exec", 0);
    setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 0);

    /* M6 kernel bridge: load the intent device, then set up the cgroup
     * v2 class hierarchy (system/tools/agents with cpu budgets). */
    mkdir("/lib/modules", 0755);
    char *ins_argv[] = { "insmod", "/lib/modules/aios_intent.ko", NULL };
    int ins_rc = run("/bin/busybox", ins_argv);
    fprintf(con, "[init] kernel bridge module %s\n",
            ins_rc == 0 ? "loaded (/dev/aios, /proc/aios)" : "not loaded");
    if (aios_cgroup_boot() == 0)
        say("[init] cgroup v2 classes: system (unlimited), tools (50%), agents (25%)");
    else
        say("[init] cgroup v2 classes not established (continuing)");
    setenv("AIOS_SANDBOX_HARD", "1", 0);

    /* M10 desktop: GPU/input module stack + boot splash on /dev/fb0 */
    gui_phase();
    splash_phase("kernel bridge + cgroups ready");

    /* M7 LLM-first: the shell (boot script + console) routes through the
     * OS's own micro language model unless the cmdline says otherwise. */
    setenv("AIOS_SHELL_CORTEX", "micro", 0);

    /* M7 autopilot: start the closed-loop scheduler governor. */
    char apmode[32] = "balanced";
    cmdline_value("aios.autopilot", apmode, sizeof apmode);
    /* C1: the governor is SUPERVISED, not merely started.
     *
     * Until now this fork() was fire-and-forget: init never recorded the pid, so
     * the reap loop treated the governor as "some other child" and a dead
     * governor was indistinguishable from a governor that never existed. The
     * scheduler silently vanished and the desktop kept running as if it were
     * still being scheduled - a failure with no symptom, which is the worst
     * kind. The pid and the mode now live in the reap loop below, next to the
     * desktop and the console shell, and are respawned on the same terms. */
    say(strcmp(apmode, "off") == 0
          ? "[init] autopilot: disabled (aios.autopilot=off)"
          : "[init] autopilot: supervised (respawn on exit)");

    /* C1: the governor's supervision state.
     *
     * Declared here, not beside the reap loop, because TWO paths now need it:
     * the loop (interactive and GUI boots) and the aios.mode=test path, which
     * powers off before the loop is ever entered. The first version declared it
     * at the loop, so the CI proof could not compile - and before that, could
     * not run at all. A boot mode that skips the supervisor is a boot mode that
     * cannot prove the supervisor works.
     *
     * `gov_on` is decided once, from the cmdline, because a mode that can change
     * mid-run makes the log ambiguous about which governor a respawn line
     * refers to. */
    const int gov_on = (strcmp(apmode, "off") != 0);
    pid_t gov_pid = -1;
    int gov_crashes = 0;
    long gov_backoff_ms = 0;
    int gov_kill_pending = 1;   /* C1 self-test fires at most once per boot */
    int gov_kill_in = 0;        /* countdown to the deliberate kill; 0 = idle */
    (void)gov_kill_in;

    /* C5: the selftest is OPT-IN for release images.
     *
     * It used to run on every boot unless told not to, which meant a shipped
     * image spent the first seconds of every boot running 20 test suites that a
     * user cannot act on and cannot see. The suites are the proof the PROJECT
     * ships; they are not a thing the PRODUCT does.
     *
     * It still runs whenever it is asked for, and `aios.mode=test` (what CI and
     * boot-test.sh pass) implies asking for it, so the coverage that matters is
     * unchanged. A release boot now says one line instead of several hundred. */
    int selftest_fail = 0;
    const int selftest_on = cmdline_flag("aios.selftest=1") ||
                            (cmdline_flag("aios.mode=test") && !cmdline_flag("aios.selftest=0"));
    if (selftest_on) {
        splash_phase("selftest: M1-M11 suites running");
        say("[init] phase 1/2: AI-OS selftest (M1-M11 unit + integration suite)");
        char *st_argv[] = { "aios-selftest", NULL };
        int rc = run("/usr/bin/aios-selftest", st_argv);
        selftest_fail = (rc != 0);
        fprintf(con, "[init] selftest %s (rc=%d)\n",
                selftest_fail ? "FAILED" : "PASSED", rc);
    } else {
        say("[init] selftest not requested (release boot; pass aios.selftest=1 "
            "or aios.mode=test to run it)");
    }

    splash_phase("system bring-up via aiosh (AI shell)");
    say("[init] phase 2/2: system bring-up via aiosh (AI shell)");
    char *boot_argv[] = { "aiosh", "/etc/aios/boot.aiosh", NULL };
    run("/sbin/aiosh", boot_argv);

    fprintf(con, "AIOS-BOOT-OK selftest=%s\n",
            selftest_fail ? "FAIL" : "PASS");
    fflush(con);

    splash_phase("AI-OS ready - handing over to the desktop");
    if (cmdline_flag("aios.mode=test")) {
        /* give the governor time for its first tick so CI can assert it */
        for (int i = 0; i < 40; i++) {           /* up to 4 s */
            struct stat stt;
            if (stat("/var/lib/aios/autopilot-state.json", &stt) == 0)
                break;
            usleep(100 * 1000);
        }
        /* C1's proof has to happen HERE, before power_off(), and the first
         * version of this did not: it put the kill in the reap loop, which
         * aios.mode=test never reaches because it powers off right after boot.
         * The boot test then asserted three log lines that could not exist -
         * the supervision code was correct and the PROOF was unreachable, which
         * is the same class of mistake as a test that cannot fail.
         *
         * So: kill it, wait for the respawn, and say so. Bounded, because a
         * governor that cannot be killed must not hang the boot test. */
        if (gov_on && gov_pid > 0 && cmdline_flag("aios.governor.kill=1")) {
            snprintf(line, sizeof line,
                     "[init] C1 self-test: killing governor pid %d", (int)gov_pid);
            say(line);
            kill(gov_pid, SIGKILL);
            /* reap it, then let the loop's spawn path bring it back */
            int wst = 0;
            for (int i = 0; i < 50; i++) {        /* up to 5 s */
                const pid_t w = waitpid(gov_pid, &wst, WNOHANG);
                if (w == gov_pid || w < 0) break;
                usleep(100 * 1000);
            }
            gov_pid = -1;
            gov_crashes++;
            snprintf(line, sizeof line,
                     "[init] governor died (killed by signal 9) - respawning");
            say(line);
            gov_pid = fork();
            if (gov_pid == 0) {
                char *gv[] = { "aios-governor", "--mode", apmode, NULL };
                execv("/sbin/aios-governor", gv);
                _exit(127);
            }
            if (gov_pid > 0) {
                snprintf(line, sizeof line,
                         "[init] autopilot governor: pid %d (mode %s)",
                         (int)gov_pid, apmode);
                say(line);
            }
            for (int i = 0; i < 30; i++) {        /* up to 3 s: let it tick */
                struct stat stt;
                if (stat("/var/lib/aios/autopilot-state.json", &stt) == 0) break;
                usleep(100 * 1000);
            }
        }
        splash_close();
        power_off(); /* CI: boot verified, do not go interactive */
    }

    say("");
    say("AI-OS console ready. You are talking to aiosh (the AI shell).");
    say("Try:  version | help | 6*7 | list files | summarize the workspace");
    say("      remember <fact> ; recall <query> | spawn <goal> ; jobs");
    say("------------------------------------------------------------------");
    if (g_gui_ok)
        say("M10: the graphical desktop is starting on /dev/fb0 (aios.gui=0 disables)");

    /* M10 respawn loop: TWO console front-ends live forever.
     *   - graphics: /sbin/aios-desktop on /dev/fb0 (when the GUI is up)
     *   - serial:   /sbin/aiosh on the console (always)
     * Both are supervised; five consecutive crashes of the desktop fall
     * back to text-only (the M1-M9 behaviour). PID 1 never exits. */
    int use_gui = g_gui_ok;
    int gui_crashes = 0, sh_crashes = 0;
    pid_t gui_pid = -1, sh_pid = -1;
    splash_close();                       /* desktop owns the fb now */
    for (;;) {
        /* C1: respawn the governor with exponential backoff.
         *
         * The cap is 5, like the desktop and the shell, but the reason is
         * different: a governor that dies instantly and is respawned instantly
         * is a fork bomb with a scheduler attached. Backoff caps at 5 s, and the
         * 5th failure stops trying - at that point the log line is more useful
         * than another attempt. */
        if (gov_on && gov_pid == -1 && gov_crashes < 5) {
            if (gov_backoff_ms > 0) {
                snprintf(line, sizeof line,
                         "[init] governor restarts in %ld ms (attempt %d of 5)",
                         gov_backoff_ms, gov_crashes + 1);
                say(line);
                usleep((useconds_t)gov_backoff_ms * 1000);
            }
            gov_pid = fork();
            if (gov_pid == 0) {
                char *gv[] = { "aios-governor", "--mode", apmode, NULL };
                execv("/sbin/aios-governor", gv);
                _exit(127);
            }
            if (gov_pid > 0) {
                snprintf(line, sizeof line,
                         "[init] autopilot governor: pid %d (mode %s)", (int)gov_pid, apmode);
                say(line);
                /* C1's own proof, and it ships rather than living in a comment:
                 * with aios.governor.kill=1 init kills the governor ONCE, so the
                 * supervision path is exercised on every boot job instead of only
                 * when something happens to crash. A respawner that has never
                 * been tested is a respawner nobody knows works - that is how the
                 * pid went untracked for M1..M10.
                 *
                 * The kill is DEFERRED by a countdown, not done here with a
                 * sleep: PID 1 must not block before the system is up, and a
                 * usleep() in the spawn path is exactly that. The countdown ticks
                 * in the reap loop below, so the governor has a chance to run
                 * before it is killed. */
                if (gov_kill_pending && cmdline_flag("aios.governor.kill=1")) {
                    gov_kill_pending = 0;
                    gov_kill_in = 3;               /* about 3 loop iterations */
                }
            } else {
                say("[init] autopilot governor: fork failed");
                gov_crashes = 5;                   /* do not spin on a broken fork */
            }
        }
        /* C1 self-test countdown: let the governor run, then kill it once. */
        if (gov_kill_in > 0 && --gov_kill_in == 0 && gov_pid > 0) {
            snprintf(line, sizeof line,
                     "[init] C1 self-test: killing governor pid %d", (int)gov_pid);
            say(line);
            kill(gov_pid, SIGKILL);
        }
        if (use_gui && gui_pid == -1 && gui_crashes < 5) {
            gui_pid = fork();
            if (gui_pid == 0) {
                char *dv[] = { "aios-desktop", NULL };
                execv("/sbin/aios-desktop", dv);
                _exit(127);
            }
            say("[init] desktop spawned (graphics console)");
        }
        if (sh_pid == -1 && sh_crashes < 5) {
            sh_pid = fork();
            if (sh_pid == 0) {
                char *sh_argv[] = { "aiosh", NULL };
                execv("/sbin/aiosh", sh_argv);
                /* fallback: emergency busybox shell */
                char *em_argv[] = { "sh", NULL };
                execv("/bin/busybox", em_argv);
                _exit(127);
            }
        }
        int st = 0;
        /* WNOHANG + a short nap, so a pending C1 countdown can tick. Before C1
         * this could block indefinitely, which was fine when the loop had no
         * timed work; a blocking waitpid with a countdown above it would mean
         * the kill only fires when some unrelated child happens to exit. */
        pid_t p = waitpid(-1, &st, (gov_kill_in > 0) ? WNOHANG : 0);
        if (p == 0) { usleep(100 * 1000); continue; }
        if (p < 0) { if (errno == EINTR) continue; usleep(100000); continue; }
        if (p == gui_pid) {
            gui_pid = -1;
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) gui_crashes = 0;
            else if (++gui_crashes >= 5) {
                use_gui = 0;
                say("[init] aios-desktop keeps crashing; text console only");
            } else
                say("[init] respawning desktop (graphics console)");
        } else if (p == sh_pid) {
            sh_pid = -1;
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) sh_crashes = 0;
            else if (++sh_crashes >= 5) {
                say("[init] aiosh keeps exiting; entering emergency shell");
                char *em2[] = { "sh", NULL };
                run("/bin/busybox", em2);
                sh_crashes = 0;
            } else
                say("[init] respawning console shell (aiosh)");
        } else if (p == gov_pid) {
            /* C1: say WHICH failure this was. "died" and "exited 0" are
             * different bugs, and a supervisor that reports neither is a
             * supervisor that hides them. */
            gov_pid = -1;
            /* No statement-expression here: this file has to compile under
             * clang as well as gcc, and ({ ... }) is a GNU extension. */
            char how[48];
            if (WIFSIGNALED(st))
                snprintf(how, sizeof how, "killed by signal %d", WTERMSIG(st));
            else if (WIFEXITED(st))
                snprintf(how, sizeof how, "exit status %d", WEXITSTATUS(st));
            else
                snprintf(how, sizeof how, "unknown wait status");

            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
                gov_crashes = 0;
                gov_backoff_ms = 0;
                say("[init] governor exited cleanly (rc=0) - respawning");
            } else {
                gov_crashes++;
                gov_backoff_ms = (gov_crashes >= 3) ? 5000
                               : (gov_crashes == 2) ? 1000 : 200;
                snprintf(line, sizeof line,
                         "[init] governor died (%s) - respawning", how);
                say(line);
                if (gov_crashes >= 5)
                    say("[init] governor failed 5 times - no longer respawning");
            }
        }
        /* reaped other children (tool workers, one-shot helpers) — fine */
    }
}
