// fbtest —— fbdev 尺寸/旋转/写入探针（Switch/tegra_fb 用）
// 用法: fbtest [--info] [--paint] [--rotate N] [--restore] [--dev /dev/fb0]
//   --info    只读，打印 var/fix 关键字段
//   --paint   mmap 后画角标+十字，然后读回校验（X 在跑时看不见，但能证明内存通路）
//   --rotate  设 var.rotate=N 并回读实际值（可能触发 tegra_dc 重配，见 docs/AUDIT-x11-kde.md §8.4）
//   --restore 把 rotate 还原成进入时的值后退出
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void dump(const char* dev, struct fb_var_screeninfo* v, struct fb_fix_screeninfo* f) {
    printf("dev        %s\n", dev);
    printf("fix.id     %s\n", f->id);
    printf("smem_len   %u  (%.2f MB)\n", f->smem_len, f->smem_len / 1048576.0);
    printf("line_len   %u\n", f->line_length);
    printf("xres/yres  %ux%u\n", v->xres, v->yres);
    printf("virtual    %ux%u\n", v->xres_virtual, v->yres_virtual);
    printf("bpp        %u\n", v->bits_per_pixel);
    printf("rotate     %u\n", v->rotate);
    printf("vmode      %u (activate=%u)\n", v->vmode, v->activate);
    printf("rgba       %u/%u %u/%u %u/%u %u/%u\n",
           v->red.offset, v->red.length, v->green.offset, v->green.length,
           v->blue.offset, v->blue.length, v->transp.offset, v->transp.length);
    size_t need = size_t(f->line_length) * v->yres;
    printf("一帧字节   %zu   剩余 %.2f MB\n", need, (double(f->smem_len) - double(need)) / 1048576.0);
    if (v->yres_virtual > v->yres)
        printf("双缓冲     可能（yres_virtual=%u > yres=%u）\n", v->yres_virtual, v->yres);
    else
        printf("双缓冲     不可能（yres_virtual == yres）\n");
}

static int paint(int fd, struct fb_var_screeninfo* v, struct fb_fix_screeninfo* f) {
    size_t len = size_t(f->line_length) * v->yres;
    unsigned char* fb = static_cast<unsigned char*>(mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }
    // 全屏填一个可辨识的深色，四角 24x24 白块，正中十字
    for (unsigned y = 0; y < v->yres; ++y) {
        unsigned* row = (unsigned*)(fb + size_t(y) * f->line_length);
        for (unsigned x = 0; x < v->xres; ++x) row[x] = 0x00202020;
    }
    unsigned white = 0x00ffffff, red = 0x00ff0000;  // rgba: red 在 bit16
    for (unsigned y = 0; y < 24; ++y)
        for (unsigned x = 0; x < 24; ++x) {
            *((unsigned*)(fb + size_t(y) * f->line_length) + x) = white;
            *((unsigned*)(fb + size_t(v->yres - 1 - y) * f->line_length) + x) = white;
            *((unsigned*)(fb + size_t(y) * f->line_length) + (v->xres - 1 - x)) = white;
            *((unsigned*)(fb + size_t(v->yres - 1 - y) * f->line_length) + (v->xres - 1 - x)) = white;
        }
    for (unsigned i = 0; i < v->xres; ++i) *((unsigned*)(fb + size_t(v->yres / 2) * f->line_length) + i) = red;
    for (unsigned i = 0; i < v->yres; ++i) *((unsigned*)(fb + size_t(i) * f->line_length) + v->xres / 2) = red;

    // 读回校验：四角 + 中心十字 + 一个角落外侧
    struct { unsigned x, y, want; const char* name; } probes[] = {
        {0, 0, white, "左上角"},
        {v->xres - 1, 0, white, "右上角"},
        {0, v->yres - 1, white, "左下角"},
        {v->xres - 1, v->yres - 1, white, "右下角"},
        {v->xres / 2, v->yres / 2, red, "正中"},
        {v->xres - 30, 100, 0x00202020, "角外底色"},
    };
    int bad = 0;
    for (auto& pr : probes) {
        unsigned got = *((unsigned*)(fb + size_t(pr.y) * f->line_length) + pr.x);
        int ok = (got & 0x00ffffff) == (pr.want & 0x00ffffff);
        printf("  %-10s (%u,%u) want=%06x got=%06x %s\n", pr.name, pr.x, pr.y, pr.want,
               got & 0x00ffffff, ok ? "OK" : "MISMATCH");
        bad += !ok;
    }
    msync(fb, len, MS_ASYNC);
    munmap(fb, len);
    printf("paint: %s（X 在跑时看不见像素，只证明 memcpy 通路与坐标是对的）\n", bad ? "失败" : "通过");
    return bad ? 1 : 0;
}

int main(int argc, char** argv) {
    const char* dev = "/dev/fb0";
    int doInfo = 0, doPaint = 0, doRotate = 0, doRestore = 0;
    unsigned rot = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--info")) doInfo = 1;
        else if (!strcmp(argv[i], "--paint")) doPaint = 1;
        else if (!strcmp(argv[i], "--restore")) doRestore = 1;
        else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) { doRotate = 1; rot = unsigned(atoi(argv[++i])); }
        else if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev = argv[++i];
        else { fprintf(stderr, "用法: %s [--info] [--paint] [--rotate N] [--restore] [--dev /dev/fbX]\n", argv[0]); return 2; }
    }
    if (!doInfo && !doPaint && !doRotate) doInfo = 1;

    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
        perror("FBIOGET_*"); close(fd); return 1;
    }
    unsigned origRot = v.rotate;
    if (doInfo || doRotate || doRestore) dump(dev, &v, &f);

    int rc = 0;
    if (doPaint) rc |= paint(fd, &v, &f);
    if (doRotate) {
        v.rotate = rot;
        if (ioctl(fd, FBIOPUT_VSCREENINFO, &v)) {
            printf("rotate=%u 被拒绝: %s\n", rot, strerror(errno));
            rc |= 1;
        } else {
            struct fb_var_screeninfo n;
            ioctl(fd, FBIOGET_VSCREENINFO, &n);
            printf("rotate=%u 写入成功；回读 rotate=%u  xres/yres=%ux%u  virtual=%ux%u\n",
                   rot, n.rotate, n.xres, n.yres, n.xres_virtual, n.yres_virtual);
        }
    }
    if (doRestore) {
        ioctl(fd, FBIOGET_VSCREENINFO, &v);
        v.rotate = origRot;
        if (ioctl(fd, FBIOPUT_VSCREENINFO, &v)) printf("还原 rotate=%u 失败: %s\n", origRot, strerror(errno));
        else printf("已还原 rotate=%u\n", origRot);
    }
    close(fd);
    return rc;
}
