/* boot_checkpoint: lights up checkpoint N (1-8) as a small colored box on /dev/graphics/fb0.
 *
 * Why this exists: adbd never comes up during a real system-boot hang on this device (confirmed
 * on-device - adb only ever appeared during a failed TWRP boot attempt, never during Android boot),
 * and /proc/last_kmsg here is actually the MTK LK/preloader log, not the Linux kernel's, so there is
 * no log channel at all for a hang during real boot. This is a log-free alternative: a handful of
 * tiny, dependency-free native binaries, each run by an init trigger at a known boot milestone, that
 * paint one more box in a row near the top-left of the screen. Nothing else is compositing to fb0
 * while the stock boot logo sits there (SurfaceFlinger/HWC never take over in the broken case), so
 * whatever boxes got drawn stay on screen untouched - the last lit box IS the diagnosis: "boot got
 * at least this far, then stopped/crashed before the next trigger fired."
 *
 * Deliberately NOT linked against EGL/GLES/dlopen - just the fb ioctl + mmap, so it cannot itself
 * be the thing that hangs or fails to load.
 *
 * Usage: boot_checkpoint <n>   (n = 1..8, picks position+color; see CHECKPOINTS table below)
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BOX 28
#define GAP 8
#define MARGIN 12

struct rgb { unsigned char r, g, b; };
/* one distinct, easy-to-tell-apart color per milestone, in boot order */
static const struct rgb COLORS[] = {
    {0,0,0},                  /* unused index 0 */
    {255,0,0},     /* 1 red:     early-init ran (init itself is alive; sanity baseline) */
    {255,128,0},   /* 2 orange:  post-fs-data (switched root, /data available) */
    {255,255,0},   /* 3 yellow:  zygote-start trigger fired */
    {0,255,0},     /* 4 green:   vendor.gralloc-4-0 (allocator) service process is up */
    {0,255,255},   /* 5 cyan:    vendor.hwcomposer-2-1 (composer) service process is up */
    {0,128,255},   /* 6 blue:    surfaceflinger service process is up */
    {160,0,255},   /* 7 purple:  zygote has forked system_server (init.svc.zygote related) */
    {255,255,255}, /* 8 white:   sys.boot_completed=1 - full boot reached */
};
#define NCHECK (sizeof(COLORS)/sizeof(COLORS[0]) - 1)

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: boot_checkpoint <1-%d>\n", (int)NCHECK); return 1; }
    int n = atoi(argv[1]);
    if (n < 1 || n > (int)NCHECK) { fprintf(stderr, "checkpoint out of range\n"); return 1; }
    const char *fbpath = argc > 2 ? argv[2] : "/dev/graphics/fb0";

    int fd = open(fbpath, O_RDWR);
    if (fd < 0) { fprintf(stderr, "open(%s): %s\n", fbpath, strerror(errno)); return 2; }
    struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        fprintf(stderr, "FBIOGET_*SCREENINFO: %s\n", strerror(errno)); close(fd); return 3;
    }
    size_t line_len = fi.line_length ? fi.line_length : (size_t)vi.xres * (vi.bits_per_pixel / 8);
    size_t fbsize = line_len * vi.yres;
    unsigned char *fb = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { fprintf(stderr, "mmap: %s\n", strerror(errno)); close(fd); return 4; }

    int ox = MARGIN + (n - 1) * (BOX + GAP);
    int oy = MARGIN;
    struct rgb c = COLORS[n];
    for (int y = oy; y < oy + BOX && y < (int)vi.yres; y++) {
        unsigned char *row = fb + (size_t)y * line_len;
        for (int x = ox; x < ox + BOX && x < (int)vi.xres; x++) {
            if (vi.bits_per_pixel == 32) {
                unsigned px = ((unsigned)c.r << vi.red.offset) | ((unsigned)c.g << vi.green.offset) | ((unsigned)c.b << vi.blue.offset);
                if (vi.transp.length) px |= (0xFFu << vi.transp.offset);
                ((unsigned*)row)[x] = px;
            } else if (vi.bits_per_pixel == 16) {
                unsigned short v = (unsigned short)(((c.r >> (8-vi.red.length)) << vi.red.offset) |
                                                     ((c.g >> (8-vi.green.length)) << vi.green.offset) |
                                                     ((c.b >> (8-vi.blue.length)) << vi.blue.offset));
                ((unsigned short*)row)[x] = v;
            }
        }
    }
    msync(fb, fbsize, MS_SYNC);
    munmap(fb, fbsize);
    close(fd);
    return 0;
}
