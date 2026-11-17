/* mkcpio.c — AI-OS boot tools: pack a staged rootfs tree into a cpio "newc"
 * archive suitable for a Linux initramfs, without requiring root.
 *
 * Usage: mkcpio <rootfs-dir> <devnodes-file>
 *
 *   <rootfs-dir>    staged tree; regular files, dirs and symlinks are archived
 *   <devnodes-file> lines: "<c|b> <major> <minor> <mode-octal> <archive-path>"
 *                   device nodes are emitted even though they don't exist on
 *                   the host filesystem (no root needed to create them)
 *
 * Output: newc cpio on stdout. Pad to 4-byte boundaries; trailer padded to a
 * full 512-byte block. Static parts of the format reference:
 *   - newc magic "070701", 13 x 8-hex-digit header fields
 *   - kernel initramfs unpacker (init/initramfs.c) accepts this form
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static unsigned long g_ino = 100;

/* Total bytes emitted so far. The final 512-block padding used to call
 * ftell(stdout), which FAILS on a pipe (glibc 2.41 returns -1 with ESPIPE
 * when stdout is not seekable — a modern-sandbox regression that made the
 * kernel complain "Initramfs unpacking failed: broken padding" and left
 * 4-516 garbage bytes past the padding). Track the count ourselves. */
static unsigned long g_written;

static void emit(FILE *out, const void *p, size_t n) {
    fwrite(p, 1, n, out);
    g_written += n;
}

static void pad4(FILE *out, unsigned long n) {
    static const char z[4] = {0, 0, 0, 0};
    int rem = (int)(n % 4);
    if (rem) emit(out, z, (size_t)(4 - rem));
}

static void emit_header(FILE *out, unsigned long mode, unsigned long filesize,
                        unsigned long devmaj, unsigned long devmin,
                        unsigned long rmaj, unsigned long rmin,
                        const char *name) {
    char h[124];
    unsigned long namesize = strlen(name) + 1;
    snprintf(h, sizeof h, "%s%08lx%08lx%08lx%08lx%08lx%08lx%08lx"
             "%08lx%08lx%08lx%08lx%08lx%08lx",
             "070701", g_ino++, mode, 0ul /*uid*/, 0ul /*gid*/, 1ul /*nlink*/,
             0ul /*mtime*/, filesize, devmaj, devmin, rmaj, rmin,
             namesize, 0ul /*check*/);
    emit(out, h, 110);
    emit(out, name, namesize);
    pad4(out, 110 + namesize);
}

static void emit_file(FILE *out, const char *path, const char *arcname,
                      const struct stat *st) {
    FILE *in = fopen(path, "rb");
    if (!in) { fprintf(stderr, "mkcpio: cannot open %s: %s\n", path, strerror(errno)); exit(1); }
    emit_header(out, (unsigned long)st->st_mode, (unsigned long)st->st_size,
                0, 0, 0, 0, arcname);
    char buf[65536];
    size_t n;
    unsigned long total = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) { emit(out, buf, n); total += n; }
    fclose(in);
    if (total != (unsigned long)st->st_size) {
        fprintf(stderr, "mkcpio: %s changed size while reading\n", path);
        exit(1);
    }
    pad4(out, total);
}

static void emit_symlink(FILE *out, const char *target, const char *arcname) {
    unsigned long len = strlen(target);
    emit_header(out, 0120777, len, 0, 0, 0, 0, arcname);
    emit(out, target, len);
    pad4(out, len);
}

static void emit_dir(FILE *out, const char *arcname) {
    emit_header(out, 040755, 0, 0, 0, 0, 0, arcname);
}

static void emit_devnode(FILE *out, char type, unsigned maj, unsigned min,
                         unsigned mode, const char *arcname) {
    unsigned long m = (type == 'c') ? 0020000ul : 0060000ul;
    m |= mode;
    emit_header(out, m, 0, 0, 0, maj, min, arcname);
}

/* Walk the staged tree; arcname is the archive path ("" for the root). */
static void walk(FILE *out, const char *base, const char *arcdir) {
    char hostpath[4096];
    snprintf(hostpath, sizeof hostpath, "%s%s%s", base,
             arcdir[0] ? "/" : "", arcdir);
    DIR *d = opendir(hostpath);
    if (!d) { fprintf(stderr, "mkcpio: opendir %s: %s\n", hostpath, strerror(errno)); exit(1); }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char hpath[4096], apath[4096];
        /* A path that does not fit is a hard error, not a silent truncation
         * (a truncated path would emit the wrong file into the initramfs). */
        int hn = snprintf(hpath, sizeof hpath, "%s/%.*s", hostpath,
                          (int)sizeof hpath - (int)strlen(hostpath) - 2, e->d_name);
        int an = snprintf(apath, sizeof apath, "%s%s%.*s", arcdir,
                          arcdir[0] ? "/" : "",
                          (int)sizeof apath - (int)strlen(arcdir) - 2, e->d_name);
        if (hn < 0 || (size_t)hn >= sizeof hpath || an < 0 || (size_t)an >= sizeof apath) {
            fprintf(stderr, "mkcpio: path too long: %s/%s\n", hostpath, e->d_name);
            exit(1);
        }
        struct stat st;
        if (lstat(hpath, &st) != 0) { fprintf(stderr, "mkcpio: lstat %s\n", hpath); exit(1); }
        if (S_ISDIR(st.st_mode)) {
            emit_dir(out, apath);
            walk(out, base, apath);
        } else if (S_ISREG(st.st_mode)) {
            emit_file(out, hpath, apath, &st);
        } else if (S_ISLNK(st.st_mode)) {
            char target[4096];
            ssize_t n = readlink(hpath, target, sizeof target - 1);
            if (n < 0) { fprintf(stderr, "mkcpio: readlink %s\n", hpath); exit(1); }
            target[n] = 0;
            emit_symlink(out, target, apath);
        } else {
            fprintf(stderr, "mkcpio: skipping special file %s\n", hpath);
        }
    }
    closedir(d);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: mkcpio <rootfs-dir> <devnodes-file>\n");
        return 2;
    }
    FILE *out = stdout;

    /* 1. device nodes first, so /dev/console exists the moment the kernel
     *    unpacks (initramfs /dev/console must be present for init's stdio). */
    FILE *dn = fopen(argv[2], "r");
    if (!dn) { fprintf(stderr, "mkcpio: cannot open %s\n", argv[2]); return 1; }
    char line[512];
    while (fgets(line, sizeof line, dn)) {
        char type; unsigned maj, min, mode; char path[256];
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, " %c %u %u %o %255s", &type, &maj, &min, &mode, path) == 5)
            emit_devnode(out, type, maj, min, mode, path);
    }
    fclose(dn);

    /* 2. the staged tree */
    walk(out, argv[1], "");

    /* 3. trailer + pad to a full 512-byte block of zeros (the kernel's
     * initramfs parser requires the padding after the trailer to be zero
     * and 4-byte aligned; it skips zeros until it finds more data). */
    emit_header(out, 0, 0, 0, 0, 0, 0, "TRAILER!!!");
    pad4(out, 110 + 11);
    static const char z512[512] = {0};
    unsigned long rem = g_written % 512;
    if (rem) emit(out, z512, 512 - rem);
    return 0;
}
