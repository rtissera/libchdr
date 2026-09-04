/* libchdr benchmark for the Waveshare RP2350-PiZero.
 *
 * Reads CHDs from the microSD and reports, per file: wall time, input and
 * output throughput, the share of time spent in storage rather than decode,
 * per-hunk latency, peak heap, and an FNV hash of the decoded output.
 *
 * The hash is the point of running on real silicon: it proves the decode is
 * byte-identical to a desktop build rather than merely completing. Storage
 * time is measured by wrapping the core_file callbacks, so "io" is real time
 * inside FatFs/SPI and "decode" is the remainder - on an MCU those two can
 * differ by an order of magnitude between files and the aggregate hides it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "ff.h"
#include "f_util.h"
#include "hw_config.h"

#include <libchdr/chd.h>
#include <libchdr/coretypes.h>

#ifndef BENCH_HUNK_CAP
#define BENCH_HUNK_CAP 0          /* 0 = every hunk */
#endif
#ifndef BENCH_PROGRESS_EVERY
#define BENCH_PROGRESS_EVERY 2000 /* 0 = silent */
#endif
/* 32KB measured as the knee on this board: 1.05-1.12x for +33KB of heap,
 * while 64KB doubles the cost for under 0.7% more and 256KB buys nothing.
 * The budget is a ceiling, so on an image whose hunks exceed it (AVHuff is
 * 223,668 bytes) caching simply stays off rather than over-allocating.
 * Set 0 to disable. */
#ifndef BENCH_CACHE_BUDGET
#define BENCH_CACHE_BUDGET (32 * 1024)
#endif

/* ---- files under test; missing entries are skipped, not fatal ---- */
static const char *const g_files[] = {
    "0:/roms/pcenginecd/Insanity (USA) (Unl).cue.chd",
    "0:/roms/pcenginecd/Pyramid Plunder (USA) (Unl).cue.chd",
    "0:/roms/pcenginecd/Hawiian Island Girls (USA) (Unl).cue.chd",
    "0:/roms/segacd/Sensible Soccer (E) (Demo).chd",
    "0:/roms/segacd/Shadowrun (J).chd",
    "0:/roms/segacd/Surgical Strike (Brazil) (32X CD).chd",
    "0:/roms/saturn/SS-parodius-sexy.chd",
    "0:/roms/mame/simpbowl/simpbowl.chd",
    "0:/roms/mame/kinst2/kinst2.chd",
    "0:/roms/dreamcast/Ikaruga (Japan).chd",
};

/* ---- peak heap by polling newlib's mallinfo ----
 * The --wrap route collides with pico_malloc's own wrappers, and on rv32 it
 * produced unexplained corruption at real-content scale where mallinfo did
 * not, so poll instead of interpose. */
static size_t g_peak, g_base;

static size_t heap_used(void)
{
    struct mallinfo mi = mallinfo();
    return (size_t)mi.uordblks;
}

static void peak_sample(void)
{
    size_t u = heap_used();
    size_t rel = (u > g_base) ? (u - g_base) : 0;
    if (rel > g_peak) g_peak = rel;
}

/* ---- core_file over FatFs, with the storage time measured ---- */
typedef struct {
    FIL      fil;
    uint64_t io_us;
    uint64_t io_bytes;
} sdfile;

static uint64_t sd_fsize(void *argp)
{
    return (uint64_t)f_size(&((sdfile *)argp)->fil);
}

static size_t sd_fread(void *ptr, size_t size, size_t nmemb, void *argp)
{
    sdfile *s = (sdfile *)argp;
    UINT br = 0;
    uint64_t t0 = time_us_64();
    FRESULT fr = f_read(&s->fil, ptr, (UINT)(size * nmemb), &br);
    s->io_us += time_us_64() - t0;
    s->io_bytes += br;
    return (fr == FR_OK) ? (br / size) : 0;
}

static int sd_fseek(void *argp, int64_t offset, int whence)
{
    sdfile *s = (sdfile *)argp;
    FSIZE_t base = (whence == SEEK_SET) ? 0
                 : (whence == SEEK_CUR) ? f_tell(&s->fil)
                                        : f_size(&s->fil);
    uint64_t t0 = time_us_64();
    FRESULT fr = f_lseek(&s->fil, base + offset);
    s->io_us += time_us_64() - t0;
    return (fr == FR_OK) ? 0 : -1;
}

static int sd_fclose(void *argp) { (void)argp; return 0; }

static const core_file_callbacks sd_callbacks = {
    .fsize = sd_fsize, .fread = sd_fread, .fclose = sd_fclose, .fseek = sd_fseek,
};

static uint64_t fnv(uint64_t h, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int run_one(const char *path)
{
    sdfile sf;
    chd_file *chd = NULL;
    chd_error err;

    memset(&sf, 0, sizeof sf);
    if (f_open(&sf.fil, path, FA_READ) != FR_OK) {
        printf("%-34s SKIP (not on card)\n", basename_of(path));
        return 0;
    }

    g_base = heap_used();
    g_peak = 0;

    err = chd_open_core_file_callbacks(&sd_callbacks, &sf, CHD_OPEN_READ, NULL, &chd);
    if (err != CHDERR_NONE) {
        printf("%-34s OPEN FAILED: %s\n", basename_of(path), chd_error_string(err));
        f_close(&sf.fil);
        return 0;
    }
#if BENCH_CACHE_BUDGET
    err = chd_set_cache_budget(chd, BENCH_CACHE_BUDGET);
    if (err != CHDERR_NONE) {
        /* not fatal: caching stays off and the file remains usable */
        printf("%-34s cache budget %u B refused: %s\n", basename_of(path),
               (unsigned)BENCH_CACHE_BUDGET, chd_error_string(err));
    } else if (chd_get_cache_budget(chd) == 0) {
        printf("%-34s cache off: hunk exceeds the %u B budget\n",
               basename_of(path), (unsigned)BENCH_CACHE_BUDGET);
    }
#endif
    peak_sample();

    const chd_header *h = chd_get_header(chd);
    uint32_t n = h->totalhunks;
#if BENCH_HUNK_CAP
    if (n > BENCH_HUNK_CAP) n = BENCH_HUNK_CAP;
#endif

    unsigned char *buf = malloc(h->hunkbytes);
    if (!buf) {
        printf("%-34s hunk buffer alloc failed (%u B)\n", basename_of(path), h->hunkbytes);
        chd_close(chd); f_close(&sf.fil);
        return 0;
    }

    uint64_t hsh = 1469598103934665603ULL;
    uint64_t io_at_start = sf.io_us;
    uint64_t t0 = time_us_64();
    uint32_t i;
    for (i = 0; i < n; i++) {
        err = chd_read(chd, i, buf);
        if (err != CHDERR_NONE) {
            printf("%-34s READ FAILED at hunk %u/%u: %s  (free %u B)\n",
                   basename_of(path), i, n, chd_error_string(err),
                   (unsigned)heap_used());
            break;
        }
        hsh = fnv(hsh, buf, h->hunkbytes);
        if ((i & 15) == 0) peak_sample();
#if BENCH_PROGRESS_EVERY
        if (i && (i % BENCH_PROGRESS_EVERY) == 0)
            printf("    ..%u/%u  %.3f ms/hunk\n", i, n,
                   (double)(time_us_64() - t0) / 1000.0 / i);
#endif
    }
    uint64_t us = time_us_64() - t0;
    uint64_t io_us = sf.io_us - io_at_start;

    uint64_t chits = 0, cmisses = 0;
    chd_get_cache_stats(chd, &chits, &cmisses);

    free(buf);
    chd_close(chd);
    f_close(&sf.fil);

    if (i != n) return 0;

    uint64_t outb = (uint64_t)h->hunkbytes * n;
    printf("%-34s hunks=%-6u hb=%-6u %8.1f ms  out=%5.2f MB/s  in=%5.2f MB/s  "
           "io=%4.1f%%  %.3f ms/hunk  peak=%6u B  %016llx\n",
           basename_of(path), n, h->hunkbytes, us / 1000.0,
           (double)outb / (double)us,
           (double)sf.io_bytes / (double)us,
           100.0 * (double)io_us / (double)us,
           (double)us / 1000.0 / n,
           (unsigned)g_peak, (unsigned long long)hsh);
    if (chits + cmisses)
        printf("%-34s   cache %u B: hit=%llu miss=%llu (%.0f%%)\n", "",
               (unsigned)BENCH_CACHE_BUDGET, (unsigned long long)chits,
               (unsigned long long)cmisses,
               100.0 * (double)chits / (double)(chits + cmisses));
    return 1;
}

static FATFS fs;

int main(void)
{
    stdio_init_all();
    for (int i = 0; i < 40 && !stdio_usb_connected(); i++) sleep_ms(250);
    sleep_ms(400);

    printf("\n=== libchdr RP2350 SD benchmark ===\n");
    printf("Cortex-M33 %u Hz  LOWRAM_TARGET=%d  hunk_cap=%d\n",
           (unsigned)clock_get_hz(clk_sys), LOWRAM_TARGET, BENCH_HUNK_CAP);

    FRESULT fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK) {
        printf("MOUNT FAILED: %s\n", FRESULT_str(fr));
        while (1) { sleep_ms(5000); printf("idle (no card)\n"); }
    }
    printf("SD mounted\n\n");

    int ok = 0, total = (int)(sizeof g_files / sizeof *g_files);
    for (int i = 0; i < total; i++)
        ok += run_one(g_files[i]);

    printf("\n=== %d/%d files OK ===\n", ok, total);
    while (1) { sleep_ms(5000); printf("idle\n"); }
}
