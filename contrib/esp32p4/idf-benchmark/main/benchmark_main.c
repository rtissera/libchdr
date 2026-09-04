/*
 * Real-hardware libchdr throughput benchmark for ESP32-P4 (Waveshare
 * ESP32-P4-NANO, dual-core RISC-V @ up to ~400MHz, 768KB HP L2MEM).
 *
 * Opens every CHD embedded into flash by gen_embed.sh (the synthetic
 * tests/corpus/seeds + tests/avhuff_corpus set, plus a few small real
 * MAME-derived CHDs pulled from an actual game library), reads every hunk
 * of each with block-CRC verification on (VERIFY_BLOCK_CRC=1, see
 * components/libchdr/CMakeLists.txt), times the decode with
 * esp_timer_get_time(), and prints per-file + aggregate MB/s (both
 * compressed-in and decompressed-out) over the USB-C serial console.
 *
 * Storage is an in-memory core_file backend over the embedded flash data
 * (same pattern as tests/esp32p4/fw.c's RAM-budget probe) - no SD card
 * needed, so this runs with zero GPIO/pin configuration. Real large CHDs
 * (segacd/psp/saturn/etc, tens to hundreds of MB) don't fit in flash this
 * way and need the SD card path instead - see contrib/esp32p4/README.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include "driver/sdmmc_host.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

#include <libchdr/chd.h>
#include <libchdr/coretypes.h>

#include "embed_includes.inc"

/* Waveshare ESP32-P4-NANO onboard TF/SD slot, native SDMMC slot 0, 4-bit,
 * per Waveshare's own 06_sdmmc example (confirmed against an independent
 * community bring-up suite: github.com/Tangerino/micropython-p4-test-suite).
 * SD card IO is powered from the P4's on-chip LDO channel 4 - without
 * enabling that, the slot times out with ESP_ERR_TIMEOUT. */
#define SD_PIN_CLK 43
#define SD_PIN_CMD 44
#define SD_PIN_D0  39
#define SD_PIN_D1  40
#define SD_PIN_D2  41
#define SD_PIN_D3  42
#define SD_LDO_CHAN 4
#define SD_MOUNT_POINT "/sdcard"

/* SD bus clock. ESP-IDF defaults to SDMMC_FREQ_DEFAULT (20MHz) when
 * max_freq_khz is left unset, which is what every measurement before the
 * bottleneck sweep used. SDMMC_FREQ_HIGHSPEED is 40MHz. */
#ifndef SD_FREQ_KHZ
#define SD_FREQ_KHZ SDMMC_FREQ_HIGHSPEED
#endif

/* ---- in-memory core_file backend (chd_open_core_file_callbacks) ---- */

typedef struct {
	const unsigned char *data;
	size_t len;
	size_t pos;
} membuf;

static uint64_t mem_fsize(void *argp)
{
	membuf *m = (membuf *)argp;
	return (uint64_t)m->len;
}

static size_t mem_fread(void *ptr, size_t size, size_t nmemb, void *argp)
{
	membuf *m = (membuf *)argp;
	size_t want = size * nmemb;
	size_t avail = (m->pos < m->len) ? (m->len - m->pos) : 0;
	size_t take = (want < avail) ? want : avail;
	memcpy(ptr, m->data + m->pos, take);
	m->pos += take;
	return take / size;
}

static int mem_fclose(void *argp)
{
	(void)argp;
	return 0;
}

static int mem_fseek(void *argp, int64_t offset, int whence)
{
	membuf *m = (membuf *)argp;
	int64_t base = (whence == SEEK_SET) ? 0 : (whence == SEEK_CUR) ? (int64_t)m->pos : (int64_t)m->len;
	int64_t newpos = base + offset;
	if (newpos < 0 || (uint64_t)newpos > m->len) return -1;
	m->pos = (size_t)newpos;
	return 0;
}

static const core_file_callbacks mem_callbacks = {
	.fsize = mem_fsize,
	.fread = mem_fread,
	.fclose = mem_fclose,
	.fseek = mem_fseek,
};

/* ---- real file core_file backend (chd_open_core_file_callbacks), for the
 * SD/mass-storage path - reads through the FATFS VFS mounted on the
 * onboard SDMMC slot, same interface libchdr sees on any other host. ---- */

static uint64_t sdfile_fsize(void *argp)
{
	FILE *f = (FILE *)argp;
	long cur = ftell(f);
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, cur, SEEK_SET);
	return (uint64_t)sz;
}

/* Bottleneck attribution without touching libchdr: these callbacks are the
 * only path from the decoder to the card, so accounting here splits a hunk's
 * wall time into "waiting on SD" and "everything else" (decode + CRC + map)
 * exactly, with no guesswork and no instrumentation inside the library. */
static struct {
	uint64_t read_us, seek_us;
	uint64_t read_bytes;
	uint64_t reads, seeks, seeks_elided;
	int64_t  pos;          /* our idea of the current file offset */
	int      pos_valid;
} g_io;

static void io_reset(void) { memset(&g_io, 0, sizeof(g_io)); }

static size_t sdfile_fread(void *ptr, size_t size, size_t nmemb, void *argp)
{
	int64_t t0 = esp_timer_get_time();
	size_t n = fread(ptr, size, nmemb, (FILE *)argp);
	g_io.read_us += (uint64_t)(esp_timer_get_time() - t0);
	g_io.read_bytes += (uint64_t)n * size;
	g_io.reads++;
	if (g_io.pos_valid) g_io.pos += (int64_t)(n * size);
	return n;
}

static int sdfile_fclose(void *argp)
{
	return fclose((FILE *)argp);
}

static int sdfile_fseek(void *argp, int64_t offset, int whence)
{
	int64_t t0, r;
	t0 = esp_timer_get_time();
	r = fseek((FILE *)argp, (long)offset, whence);
	g_io.seek_us += (uint64_t)(esp_timer_get_time() - t0);
	g_io.seeks++;
	if (r == 0 && whence == SEEK_SET) { g_io.pos = offset; g_io.pos_valid = 1; }
	else g_io.pos_valid = 0;
	return (int)r;
}

/* ---- FatFs-backed core_file, bypassing VFS and newlib stdio ----
 *
 * Measured on this board, same card and clock: sdmmc_read_sectors() sustains
 * 18.96 MB/s, FatFs f_read() 10.58, and fread() through the VFS and newlib
 * stdio only 2.30. The wrapper above FatFs costs 4.6x.
 *
 * libchdr never needs to know: core_file_callbacks already lets the embedder
 * supply whatever reader it likes, so this is a drop-in replacement for the
 * stdio backend and requires no change to the library. */
/* Use the FatFs-backed core_file instead of the stdio one. */
#ifndef BENCH_FATFS_BACKEND
#define BENCH_FATFS_BACKEND 0
#endif

#if BENCH_FATFS_BACKEND
static uint64_t fffile_fsize(void *argp)
{
	return (uint64_t)f_size((FIL *)argp);
}

static size_t fffile_fread(void *ptr, size_t size, size_t nmemb, void *argp)
{
	UINT got = 0;
	int64_t t0 = esp_timer_get_time();
	FRESULT r = f_read((FIL *)argp, ptr, (UINT)(size * nmemb), &got);
	g_io.read_us += (uint64_t)(esp_timer_get_time() - t0);
	g_io.read_bytes += got;
	g_io.reads++;
	if (r != FR_OK) return 0;
	return size ? (got / size) : 0;
}

static int fffile_fclose(void *argp)
{
	FIL *fp = (FIL *)argp;
	DWORD *clmt = fp->cltbl;
	FRESULT r;
	fp->cltbl = NULL;
	r = f_close(fp);
	free(clmt);
	free(fp);
	return (r == FR_OK) ? 0 : -1;
}

static int fffile_fseek(void *argp, int64_t offset, int whence)
{
	FIL *fp = (FIL *)argp;
	FSIZE_t target;
	int64_t t0;
	FRESULT r;

	switch (whence) {
	case SEEK_SET: target = (FSIZE_t)offset; break;
	case SEEK_CUR: target = f_tell(fp) + (FSIZE_t)offset; break;
	case SEEK_END: target = f_size(fp) + (FSIZE_t)offset; break;
	default: return -1;
	}
	t0 = esp_timer_get_time();
	r = f_lseek(fp, target);
	g_io.seek_us += (uint64_t)(esp_timer_get_time() - t0);
	g_io.seeks++;
	return (r == FR_OK) ? 0 : -1;
}

static const core_file_callbacks fffile_callbacks = {
	.fsize = fffile_fsize,
	.fread = fffile_fread,
	.fclose = fffile_fclose,
	.fseek = fffile_fseek,
};

/* opens a FatFs path (the VFS path minus the mount prefix) */
/* CLMT size for the FatFs backend. Every file on this card is a single
 * fragment, so 3 words suffice; sized well above that so fragmented cards
 * still get fast seek. 512 words is 2KB per open file. */
#define FFFILE_CLMT_WORDS 512

static FIL *fffile_open(const char *vfs_path)
{
	const char *ff = vfs_path;
	FIL *fp;
	DWORD *clmt;

	if (strncmp(ff, SD_MOUNT_POINT, strlen(SD_MOUNT_POINT)) == 0)
		ff += strlen(SD_MOUNT_POINT);
	fp = (FIL *)malloc(sizeof(FIL));
	if (fp == NULL) return NULL;
	if (f_open(fp, ff, FA_READ) != FR_OK) { free(fp); return NULL; }

	/* Build the cluster link map ourselves. Going straight to FatFs skips
	 * ESP-IDF's VFS, and the VFS is what normally allocates cltbl and runs
	 * f_lseek(CREATE_LINKMAP) - see vfs_fat.c. Without it every *backward*
	 * seek restarts FatFs's cluster-chain walk from the first cluster, and
	 * every COMPRESSION_SELF reference is a backward seek, so a self-ref-heavy
	 * CHD slows to a crawl. That is the same pathology CONFIG_FATFS_USE_FASTSEEK
	 * was enabled to cure, and it is easy to miss: a short capped run never
	 * walks far enough to notice. */
	clmt = (DWORD *)malloc(sizeof(DWORD) * FFFILE_CLMT_WORDS);
	if (clmt != NULL) {
		fp->cltbl = clmt;
		clmt[0] = FFFILE_CLMT_WORDS;
		if (f_lseek(fp, CREATE_LINKMAP) != FR_OK) {
			/* too fragmented to map - run without fast seek rather than fail */
			printf("  (fast seek unavailable, needs %lu words)\n", (unsigned long)clmt[0]);
			fp->cltbl = NULL;
			free(clmt);
		}
		f_lseek(fp, 0);
	}
	return fp;
}
#endif /* BENCH_FATFS_BACKEND */

static const core_file_callbacks sdfile_callbacks = {
	.fsize = sdfile_fsize,
	.fread = sdfile_fread,
	.fclose = sdfile_fclose,
	.fseek = sdfile_fseek,
};

/* mounts the onboard TF/SD slot; returns the card handle on success, NULL
 * (with a logged reason) if no card is present or the mount fails - this
 * benchmark still runs the flash-embedded corpus either way. */
static sdmmc_card_t *mount_sdcard(void)
{
	esp_err_t ret;
	sdmmc_card_t *card = NULL;

	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = false,
		.max_files = 8,
		.allocation_unit_size = 16 * 1024,
	};

	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
	host.slot = SDMMC_HOST_SLOT_0;
	host.max_freq_khz = SD_FREQ_KHZ;

#if SOC_SDMMC_IO_POWER_EXTERNAL
	sd_pwr_ctrl_ldo_config_t ldo_config = { .ldo_chan_id = SD_LDO_CHAN };
	sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
	ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
	if (ret != ESP_OK) {
		printf("SD: sd_pwr_ctrl_new_on_chip_ldo failed: %s\n", esp_err_to_name(ret));
		return NULL;
	}
	host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

	sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
	slot_config.width = 4;
	slot_config.clk = SD_PIN_CLK;
	slot_config.cmd = SD_PIN_CMD;
	slot_config.d0 = SD_PIN_D0;
	slot_config.d1 = SD_PIN_D1;
	slot_config.d2 = SD_PIN_D2;
	slot_config.d3 = SD_PIN_D3;
	slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

	ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
	if (ret != ESP_OK) {
		printf("SD: mount failed (%s) - is a card inserted?\n", esp_err_to_name(ret));
		return NULL;
	}

	sdmmc_card_print_info(stdout, card);
	return card;
}

/* recursively finds every *.chd under root, up to max_files entries; each
 * found path is stored as a heap-allocated string in out[]. Returns the
 * count found. */
#define SD_MAX_FILES 128
#define SD_MAX_DEPTH 6

/* Per-file hunk cap for the SD sweep (0 = read every hunk). A full uncapped
 * sweep of all 292 valid CHDs on the card is ~151GB decompressed, ~24h. Pass A
 * instead runs uncapped over the characterized sample below, ~6.8GB. */
#ifdef BENCH_HUNK_CAP
#define SD_MAX_HUNKS_PER_FILE BENCH_HUNK_CAP
#else
#define SD_MAX_HUNKS_PER_FILE 0
#endif

static uint32_t g_max_hunks = 0;

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* BENCH_PROGRESS_EVERY: emit a progress line every N hunks. Off by default.
 * Without it a stalled run is indistinguishable from a merely slow one - the
 * board just stops printing, and "hung" vs "still going, 40x slower than it
 * started" are completely different diagnoses. With it, the slope of ms/hunk
 * over the file separates a driver stall (flat, then nothing) from map-window
 * thrashing (progressively degrading). */
#ifndef BENCH_PROGRESS_EVERY
#define BENCH_PROGRESS_EVERY 0
#endif

/* Read-ahead budget handed to libchdr per file. 0 keeps the historical
 * one-read-per-hunk behaviour, which is the control this run needs. */
#ifndef BENCH_CACHE_BUDGET
#define BENCH_CACHE_BUDGET 0
#endif

/* Quick validation subset: the five files that between them are the only
 * ones exercising each thing under test - cdzs context sharing (Ikaruga),
 * the huffman fix and the LOWRAM map-read floor (kinst2), cdzl base streams
 * (Shadowrun), the most I/O-bound file (Castlevania X) and the smallest
 * hunks / most transactions (Bonk III). */
#ifndef BENCH_QUICK
#define BENCH_QUICK 0
#endif


/* ---- filesystem geometry / fragmentation ----
 *
 * Whether FATFS_USE_FASTSEEK does anything at all depends on how many
 * contiguous cluster runs a file occupies: the CLMT buffer is fixed size
 * (CONFIG_FATFS_FAST_SEEK_BUFFER_SIZE, 64 DWORDs by default = ~31 runs) and
 * ESP-IDF silently falls back to the slow path when a file needs more, with
 * no error and no log line. So measure it rather than assume.
 *
 * f_lseek(CREATE_LINKMAP) writes the *required* table size into cltbl[0] even
 * when it fails with FR_NOT_ENOUGH_CORE, which is exactly the number wanted.
 * Table layout is [size][n0][c0][n1][c1]... so runs = (items - 1) / 2. */


/* ---- Pass A sample ----
 *
 * Picked by characterizing all 292 valid CHDs on the card (see
 * tools/characterize.c) and then covering every axis that actually varies,
 * rather than taking "a few files per system" and hoping. Between them these
 * 14 cover:
 *   geometry   all 5 hunkbytes/units-per-hunk combinations in the corpus
 *              (19584/8, 4096/8, 9792/4, 2448/1, 4096/2)
 *   codecs     all 11 selectors seen anywhere: cdlz cdzl cdfl cdzs zlib lzma
 *              huff flac zstd, plus uncompressed and self-referencing hunks
 *   codec mix  files using 1, 2, 3 and 4 distinct codecs
 *   tracks     0, 1, 2, 3, 13, 17 and 96 tracks
 *   size       2MB to 1.25GB logical
 *   systems    all 7 present on the card
 *
 * Two entries are deliberately non-obvious: Bonk III is the only 1-unit-per-
 * hunk file in the corpus, so it is the control for the read-amplification
 * sweep (no amplification is possible), and Shadowrun is the most cdzl-heavy
 * file at 58.7% - cdzl being the codec the ESP ROM miniz collision broke, so
 * it is worth keeping permanently in the regression path. */
#define SD_USE_SAMPLE 1

/* Pass B is cheap (a few minutes) but only meaningful on the sample */
#define PASS_B_ENABLE 1

#if BENCH_QUICK
static const char *const g_sd_sample[] = {
	"/sdcard/roms/psp/Castlevania X.chd",
	"/sdcard/roms/dreamcast/Ikaruga (Japan).chd",
	"/sdcard/roms/mame/kinst2/kinst2.chd",
	"/sdcard/roms/segacd/Shadowrun (J).chd",
	"/sdcard/roms/pcenginecd/Bonk III - Bonk's Big Adventure (USA).chd",
};
#elif defined(BENCH_ONE_FILE)
/* single-file bisect mode, for chasing a hang down to one configuration */
static const char *const g_sd_sample[] = { BENCH_ONE_FILE };
#else
static const char *const g_sd_sample[] = {
	/* system      geometry  ncodec  tracks  dominant mix */
	"/sdcard/roms/dreamcast/Ikaruga (Japan).chd",                          /* 19584/8  2  3   cdzs 92.9% */
	"/sdcard/roms/mame/kinst2/kinst2.chd",                                 /*  4096/8  4  0   lzma+zlib+huff+flac */
	"/sdcard/roms/mame/simpbowl/simpbowl.chd",                             /*  9792/4  3  1   cdlz 55.6% cdzl 36.8% */
	"/sdcard/roms/naomi/vathlete/gds-0019.chd",                            /* 19584/8  3  3   cdlz 88.6%, largest */
	"/sdcard/roms/pcenginecd/Bonk III - Bonk's Big Adventure (USA).chd",   /*  2448/1  2 17   cdfl 93.6%, no amplification */
	"/sdcard/roms/pcenginecd/Hawiian Island Girls (USA) (Unl).cue.chd",    /* 19584/8  1  1   cdlz 100%, tiny */
	"/sdcard/roms/pcenginecd/Insanity (USA) (Unl).cue.chd",                /* 19584/8  3 13   cdfl 79.0% */
	"/sdcard/roms/pcenginecd/Pyramid Plunder (USA) (Unl).cue.chd",         /* 19584/8  2  1   cdlz 97.6%, smallest */
	"/sdcard/roms/psp/Castlevania X.chd",                                  /*  4096/2  2  0   zstd 35.6% uncomp 34.8% */
	"/sdcard/roms/saturn/SS-parodius-sexy.chd",                            /* 19584/8  3  2   cdlz 93.6% */
	"/sdcard/roms/segacd/Cadillacs & Dinosaurs - The Second Cataclysm (U).chd", /* 19584/8 1 2 cdlz 100% */
	"/sdcard/roms/segacd/Sensible Soccer (E) (Demo).chd",                  /* 19584/8  3 96   cdfl 64.6%, most tracks */
	"/sdcard/roms/segacd/Shadowrun (J).chd",                               /* 19584/8  3  3   cdzl 58.7% */
	"/sdcard/roms/segacd/Surgical Strike (Brazil) (32X CD).chd",           /* 19584/8  3  3   cdlz 90.6% */
};
#endif

__attribute__((unused))
static int sd_scan_dir(const char *dir, char **out, int count, int depth)
{
	if (depth > SD_MAX_DEPTH || count >= SD_MAX_FILES)
		return count;

	DIR *d = opendir(dir);
	if (!d) return count;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && count < SD_MAX_FILES) {
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;

		char path[300];
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

		struct stat st;
		if (stat(path, &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			count = sd_scan_dir(path, out, count, depth + 1);
		} else {
			size_t plen = strlen(path);
			if (plen > 4 && !strcasecmp(path + plen - 4, ".chd"))
				out[count++] = strdup(path);
		}
	}
	closedir(d);
	return count;
}

/* ---- corpus table ---- */

typedef struct {
	const char *name;
	const unsigned char *data;
	unsigned int len;
} corpus_entry;

static const corpus_entry g_corpus[] = {
#include "embed_list.inc"
};


/* ---- latency histogram ----
 *
 * A mean hunk-decode time is close to useless for judging whether a CD read
 * will glitch audio - that is a tail property. Keeping every sample is not an
 * option either (271328 hunks in one corpus file), so accumulate into a
 * fixed-size log-scale histogram and read percentiles back off it.
 *
 * Bucket index packs 4 sub-buckets per octave: (log2(us) << 2) | top 2
 * mantissa bits. That is ~19% worst-case bucket width, which is plenty to
 * separate a cache-ish hit from a full LZMA hunk decode. */
#define LAT_SUBBITS 2
#define LAT_SUB     (1u << LAT_SUBBITS)
#define LAT_BUCKETS (32 * LAT_SUB)

typedef struct {
	uint32_t bucket[LAT_BUCKETS];
	uint64_t count, sum_us, min_us, max_us;
} lat_hist;

static void lat_init(lat_hist *h)
{
	memset(h, 0, sizeof(*h));
	h->min_us = UINT64_MAX;
}

static void lat_add(lat_hist *h, uint64_t us)
{
	unsigned idx, e;
	if (us == 0) us = 1;
	e = 31u - (unsigned)__builtin_clz((uint32_t)(us > 0xFFFFFFFFu ? 0xFFFFFFFFu : us));
	if (e >= LAT_SUBBITS)
		idx = (e << LAT_SUBBITS) | (unsigned)((us >> (e - LAT_SUBBITS)) & (LAT_SUB - 1));
	else
		idx = (unsigned)us;
	if (idx >= LAT_BUCKETS) idx = LAT_BUCKETS - 1;
	h->bucket[idx]++;
	h->count++;
	h->sum_us += us;
	if (us < h->min_us) h->min_us = us;
	if (us > h->max_us) h->max_us = us;
}

/* lower edge of a bucket, in us - report the conservative (low) end */
static uint64_t lat_bucket_us(unsigned idx)
{
	unsigned e = idx >> LAT_SUBBITS;
	if (e < LAT_SUBBITS) return idx;
	return ((uint64_t)(LAT_SUB | (idx & (LAT_SUB - 1)))) << (e - LAT_SUBBITS);
}

static uint64_t lat_pct(const lat_hist *h, double p)
{
	uint64_t want, seen = 0;
	unsigned i;
	if (h->count == 0) return 0;
	want = (uint64_t)(p * (double)h->count);
	for (i = 0; i < LAT_BUCKETS; i++) {
		seen += h->bucket[i];
		if (seen >= want) return lat_bucket_us(i);
	}
	return h->max_us;
}

static void lat_print(const char *label, const lat_hist *h)
{
	if (h->count == 0) { printf("  %-22s (no samples)\n", label); return; }
	printf("  %-22s n=%-7" PRIu64 " min=%-7" PRIu64 " p50=%-7" PRIu64 " p95=%-8" PRIu64
	       " p99=%-8" PRIu64 " max=%-8" PRIu64 " mean=%" PRIu64 "  (us)\n",
		label, h->count, h->min_us, lat_pct(h, 0.50), lat_pct(h, 0.95),
		lat_pct(h, 0.99), h->max_us, h->sum_us / h->count);
}

/* ---- Pass B: request-granularity / latency characterization ----
 *
 * libchdr's only read entry point is chd_read(chd, hunknum, buf) - there is no
 * sub-hunk API and no decoded-hunk cache (file_cache precaches the whole
 * *compressed* file, hopeless for a 500MB CHD here; v5_resume_cache is a
 * sequential map/codec fast path, not a data cache). So a caller asking for N
 * units must decode every hunk those units land in and slice.
 *
 * That means "latency vs request size" is really *read amplification*: at 8
 * units per hunk a 1-unit request costs a full hunk, i.e. 8x the bytes it
 * wanted. This measures what that actually costs in wall time, and how much
 * worse it gets when the request is not hunk-aligned (16 units at unit offset
 * 1 spans 3 hunks, not 2).
 *
 * Random vs sequential is measured alongside because v5_resume_cache only
 * helps sequential access, and nothing has ever quantified what it is worth. */

#define PASSB_SAMPLES 120

static uint32_t rnd_state = 0x1234567u;
static uint32_t rnd_next(void)
{
	rnd_state ^= rnd_state << 13;
	rnd_state ^= rnd_state >> 17;
	rnd_state ^= rnd_state << 5;
	return rnd_state;
}

__attribute__((unused))
static void pass_b_file(const char *name)
{
	static const uint32_t sizes[] = { 1, 2, 4, 8, 16 };
	FILE *f = fopen(name, "rb");
	chd_file *chd = NULL;
	if (!f) { printf("  %-40s fopen FAILED\n", name); return; }
	if (chd_open_core_file_callbacks(&sdfile_callbacks, f, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE) {
		printf("  %-40s OPEN FAILED\n", name);
		return;
	}
	const chd_header *h = chd_get_header(chd);
	uint32_t uph = h->unitbytes ? h->hunkbytes / h->unitbytes : 1;
	uint8_t *buf = malloc(h->hunkbytes);
	if (!buf) { printf("  %-40s malloc failed\n", name); chd_close(chd); return; }

	printf("\n  %s\n    hunkbytes=%" PRIu32 " unitbytes=%" PRIu32 " units/hunk=%" PRIu32
	       " totalhunks=%" PRIu32 "\n", name, h->hunkbytes, h->unitbytes, uph, h->totalhunks);

	/* sequential vs random, whole hunks - isolates v5_resume_cache's value */
	{
		lat_hist seq, rnd;
		lat_init(&seq); lat_init(&rnd);
		uint32_t n = h->totalhunks < PASSB_SAMPLES ? h->totalhunks : PASSB_SAMPLES;
		uint32_t base = h->totalhunks > n ? (rnd_next() % (h->totalhunks - n)) : 0;
		for (uint32_t i = 0; i < n; i++) {
			int64_t a = esp_timer_get_time();
			if (chd_read(chd, base + i, buf) != CHDERR_NONE) break;
			lat_add(&seq, (uint64_t)(esp_timer_get_time() - a));
		}
		for (uint32_t i = 0; i < n; i++) {
			uint32_t hn = rnd_next() % h->totalhunks;
			int64_t a = esp_timer_get_time();
			if (chd_read(chd, hn, buf) != CHDERR_NONE) break;
			lat_add(&rnd, (uint64_t)(esp_timer_get_time() - a));
		}
		lat_print("1 hunk sequential", &seq);
		lat_print("1 hunk random", &rnd);
		if (seq.count && rnd.count)
			printf("    -> random/sequential mean = %.2fx\n",
				(double)(rnd.sum_us / rnd.count) / (double)(seq.sum_us / seq.count));
	}

	/* request-size sweep, aligned and unaligned, random placement */
	printf("    %-9s %-9s %8s %8s %9s %9s %7s\n",
		"units", "align", "hunks/req", "p50(us)", "p95(us)", "max(us)", "amp");
	for (size_t s = 0; s < ARRAY_LEN(sizes); s++) {
		for (int unaligned = 0; unaligned < 2; unaligned++) {
			lat_hist lh;
			uint64_t touched = 0, delivered = 0, decoded = 0;
			uint32_t n = PASSB_SAMPLES;
			lat_init(&lh);
			for (uint32_t i = 0; i < n; i++) {
				/* pick a unit offset; unaligned deliberately straddles */
				uint64_t totalunits = (uint64_t)h->totalhunks * uph;
				if (totalunits <= sizes[s]) break;
				uint64_t uoff = rnd_next() % (totalunits - sizes[s]);
				if (!unaligned) uoff -= uoff % uph;
				else if (uph > 1 && (uoff % uph) == 0) uoff += 1;
				uint32_t first = (uint32_t)(uoff / uph);
				uint32_t last = (uint32_t)((uoff + sizes[s] - 1) / uph);
				if (last >= h->totalhunks) continue;
				int64_t a = esp_timer_get_time();
				int ok = 1;
				for (uint32_t hn = first; hn <= last; hn++)
					if (chd_read(chd, hn, buf) != CHDERR_NONE) { ok = 0; break; }
				if (!ok) break;
				lat_add(&lh, (uint64_t)(esp_timer_get_time() - a));
				touched += (last - first + 1);
				decoded += (uint64_t)(last - first + 1) * h->hunkbytes;
				delivered += (uint64_t)sizes[s] * h->unitbytes;
			}
			if (!lh.count) continue;
			printf("    %-9" PRIu32 " %-9s %8.2f %8" PRIu64 " %9" PRIu64 " %9" PRIu64 " %6.2fx\n",
				sizes[s], unaligned ? "unaligned" : "aligned",
				(double)touched / (double)lh.count,
				lat_pct(&lh, 0.50), lat_pct(&lh, 0.95), lh.max_us,
				delivered ? (double)decoded / (double)delivered : 0);
		}
	}

	free(buf);
	chd_close(chd);
}

/* ---- benchmark driver ---- */

typedef struct {
	uint64_t compressed_bytes;
	uint64_t decompressed_bytes;
	uint64_t elapsed_us;
	int ok;
} run_result;

static run_result run_one(const char *name, const core_file_callbacks *cb, void *argp, uint64_t reported_len)
{
	run_result r = {0};
	chd_file *chd = NULL;
	chd_error err;
	int64_t t0, t1;

	io_reset();
	/* Heap accounting. An earlier version of this used
	 * heap_caps_get_minimum_free_size(), which is the low-water mark *since
	 * boot* - it never rises again, so after the first corpus file every
	 * delta read as zero and the whole measurement was silently useless.
	 * Use free-size deltas instead and track the minimum by sampling during
	 * the read loop: free_size is a sum of per-region counters, cheap enough
	 * to poll. What matters for "does this CHD fit on a smaller part" is the
	 * resident cost of an open file plus its decode working set. */
	size_t heap_at_entry = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
	size_t heap_after_open = heap_at_entry;
	size_t heap_min_free = heap_at_entry;
	size_t largest_min = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
	t0 = esp_timer_get_time();

	err = chd_open_core_file_callbacks(cb, argp, CHD_OPEN_READ, NULL, &chd);
	if (err != CHDERR_NONE) {
		printf("%-48s OPEN FAILED: %s\n", name, chd_error_string(err));
		/* chd_open_core_file_callbacks()'s cleanup: path calls chd_close()
		 * (hence core_fclose(argp)) on every failure past its first,
		 * near-unfailable malloc(sizeof(chd_file)) - i.e. argp is already
		 * closed here in every failure mode this benchmark actually hits.
		 * An extra cb->fclose(argp) here is a double-close: harmless on the
		 * flash path (mem_fclose() is a no-op) but a real double-free of
		 * the FATFS file object on the SD path, which corrupts a FreeRTOS
		 * queue used by the VFS/SDMMC layer and hard-resets the board (seen
		 * live: an OOM opening a large real CHD from SD, immediately
		 * followed by "assert failed: xQueueSemaphoreTake queue.c:1713"). */
		return r;
	}

#if BENCH_CACHE_BUDGET
	{
		chd_error be = chd_set_cache_budget(chd, BENCH_CACHE_BUDGET);
		if (be != CHDERR_NONE)
			printf("  (cache budget %d refused: %s)\n", (int)BENCH_CACHE_BUDGET, chd_error_string(be));
	}
#endif
	heap_after_open = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
	if (heap_after_open < heap_min_free) heap_min_free = heap_after_open;

	const chd_header *header = chd_get_header(chd);
	unsigned char *buf = malloc(header->hunkbytes);
	if (!buf) {
		printf("%-48s malloc(%" PRIu32 ") failed\n", name, header->hunkbytes);
		void *retry = heap_caps_malloc(header->hunkbytes, MALLOC_CAP_DEFAULT);
		printf("  retry heap_caps_malloc(same size, DEFAULT) = %p\n", retry);
		if (retry) heap_caps_free(retry);
		heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);
		chd_close(chd);
		return r;
	}

	/* Once the ROM-miniz collision was fixed, files that used to bail out at
	 * hunk 0-few now decode in full, and a 128-file uncapped SD sweep of a
	 * real 43GB ROM set runs for hours. g_max_hunks caps the per-file read so
	 * every file in the corpus still gets exercised in one sitting; 0 = read
	 * everything (what the flash corpus and the throughput table use). */
	uint32_t nhunks = header->totalhunks;
	int capped = 0;
	if (g_max_hunks != 0 && nhunks > g_max_hunks) { nhunks = g_max_hunks; capped = 1; }

	/* Per-hunk latency comes free here - we are already in the read loop, and
	 * this is the sequential access pattern, i.e. the one v5_resume_cache's
	 * fast path is built for. Pass B measures random access against it. */
	lat_hist seqlat;
	lat_init(&seqlat);

	int bad = 0;
	uint32_t bad_hunk = 0;
#if BENCH_PROGRESS_EVERY
	int64_t prog_t = esp_timer_get_time();
	uint64_t prog_io = 0;
#endif
	for (uint32_t i = 0; i < nhunks; i++) {
		int64_t h0, h1;
#if BENCH_PROGRESS_EVERY
		if (i && (i % BENCH_PROGRESS_EVERY) == 0) {
			int64_t now = esp_timer_get_time();
			uint64_t io_now = g_io.read_us + g_io.seek_us;
			printf("    ..hunk %" PRIu32 "/%" PRIu32 "  %.3f ms/hunk  io %.3f ms/hunk  heap used %d KB\n",
				i, nhunks,
				(double)(now - prog_t) / 1000.0 / BENCH_PROGRESS_EVERY,
				(double)(io_now - prog_io) / 1000.0 / BENCH_PROGRESS_EVERY,
				(int)((heap_at_entry - heap_caps_get_free_size(MALLOC_CAP_DEFAULT)) / 1024));
			prog_t = now; prog_io = io_now;
		}
#endif
		memset(buf, 0xAA, header->hunkbytes); /* poison, so a no-op decode is visible */
		if ((i & 63) == 0) {
			size_t f = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
			size_t lb = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
			if (f < heap_min_free) heap_min_free = f;
			if (lb < largest_min) largest_min = lb;
		}
		h0 = esp_timer_get_time();
		err = chd_read(chd, i, buf);
		h1 = esp_timer_get_time();
		if (err != CHDERR_NONE) { bad = 1; bad_hunk = i; break; }
		lat_add(&seqlat, (uint64_t)(h1 - h0));
	}

	t1 = esp_timer_get_time();

	if (bad) {
		printf("%-48s READ FAILED at hunk %" PRIu32 "/%" PRIu32 ": %s\n",
			name, bad_hunk, header->totalhunks, chd_error_string(err));
		printf("  dest[0..31] = ");
		for (int k = 0; k < 32 && k < (int)header->hunkbytes; k++) printf("%02x", buf[k]);
		printf("\n");
		free(buf);
		chd_close(chd);
		return r;
	}

	/* only a full read has a meaningful compressed-input size to rate against */
	r.compressed_bytes = capped ? 0 : reported_len;
	r.decompressed_bytes = (uint64_t)header->hunkbytes * nhunks;
	r.elapsed_us = (uint64_t)(t1 - t0);
	r.ok = 1;

	double secs = r.elapsed_us / 1e6;
	double out_mbps = secs > 0 ? (r.decompressed_bytes / 1e6) / secs : 0;
	double in_mbps = secs > 0 ? (r.compressed_bytes / 1e6) / secs : 0;

	if (capped)
		printf("%-48s hunks=%" PRIu32 "/%-4" PRIu32 " (capped) hunkbytes=%-7" PRIu32 " %8.2f ms  out=%7.2f MB/s\n",
			name, nhunks, header->totalhunks, header->hunkbytes, secs * 1000.0, out_mbps);
	else
		printf("%-48s hunks=%-4" PRIu32 " hunkbytes=%-7" PRIu32 " %8.2f ms  in=%6.2f MB/s  out=%7.2f MB/s\n",
			name, header->totalhunks, header->hunkbytes, secs * 1000.0, in_mbps, out_mbps);

	/* bottleneck split: everything not spent inside the storage callbacks is
	 * decode + CRC + map work, so this attributes the hunk time exactly */
	{
		uint64_t io_us = g_io.read_us + g_io.seek_us;
		double io_pct = r.elapsed_us ? 100.0 * (double)io_us / (double)r.elapsed_us : 0;
		printf("  io: %6.2f%% of wall (%" PRIu64 " ms read + %" PRIu64 " ms seek, %"
		       PRIu64 " reads / %" PRIu64 " seeks (%" PRIu64 " elided), %" PRIu64 " KB, %.2f MB/s while reading)"
		       "   cpu: %6.2f%%\n",
			io_pct, g_io.read_us / 1000, g_io.seek_us / 1000, g_io.reads, g_io.seeks, g_io.seeks_elided,
			g_io.read_bytes / 1024,
			g_io.read_us ? (g_io.read_bytes / 1e6) / (g_io.read_us / 1e6) : 0,
			100.0 - io_pct);
		printf("  heap: open %d KB, peak %d KB (incl. %" PRIu32 " KB dest buf), "
		       "smallest largest-free-block %d KB\n",
			(int)((heap_at_entry - heap_after_open) / 1024),
			(int)((heap_at_entry - heap_min_free) / 1024),
			header->hunkbytes / 1024,
			(int)(largest_min / 1024));
		{
			uint64_t rh = 0, rm = 0;
			chd_get_cache_stats(chd, &rh, &rm);
			if (rh || rm)
				printf("  readahead: %" PRIu64 " hits / %" PRIu64 " refills\n", rh, rm);
		}
		lat_print("hunk latency (seq)", &seqlat);
	}

	free(buf);
	chd_close(chd);
	return r;
}


void app_main(void)
{
	printf("=== libchdr ESP32-P4 real-hardware throughput benchmark ===\n");
	printf("free heap: %u bytes (largest block: %u)\n",
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
		(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
	printf("free INTERNAL: %u (largest %u)  free 8BIT: %u (largest %u)\n",
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
		(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
		(unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
		(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

	/* isolate: does a single ~220KB plain malloc() work on a totally fresh
	 * heap, before any codec has run? */
	{
		void *probe = malloc(223668);
		printf("cold-boot malloc(223668) = %p\n", probe);
		void *probe_caps = heap_caps_malloc(223668, MALLOC_CAP_DEFAULT);
		printf("cold-boot heap_caps_malloc(223668, DEFAULT) = %p\n", probe_caps);
		if (probe) free(probe);
		if (probe_caps) heap_caps_free(probe_caps);
	}
	printf("%-48s %-10s %-9s %13s %11s %14s\n",
		"file", "hunks", "hunkbytes", "time", "in", "out");

	size_t n = sizeof(g_corpus) / sizeof(g_corpus[0]);
	uint64_t total_in = 0, total_out = 0, total_us = 0;
	int total_ok = 0;

	printf("--- flash-embedded corpus (%zu files) ---\n", n);
	for (size_t i = 0; i < n; i++) {
		printf("  [before] free=%u largest=%u\n",
			(unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
			(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
		bool heap_ok_before = heap_caps_check_integrity_all(true);
		membuf mb = { g_corpus[i].data, g_corpus[i].len, 0 };
		run_result r = run_one(g_corpus[i].name, &mem_callbacks, &mb, g_corpus[i].len);
		bool heap_ok_after = heap_caps_check_integrity_all(true);
		if (!heap_ok_before || !heap_ok_after) {
			printf("  ^^^ heap corruption detected: before=%d after=%d (free=%u largest=%u)\n",
				heap_ok_before, heap_ok_after,
				(unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
				(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
		}
		if (r.ok) {
			total_in += r.compressed_bytes;
			total_out += r.decompressed_bytes;
			total_us += r.elapsed_us;
			total_ok++;
		}
	}

	double total_secs = total_us / 1e6;
	printf("=== flash: %d/%zu files OK, %" PRIu64 " bytes in / %" PRIu64 " bytes out, "
		"%.2f s total, avg in=%.2f MB/s out=%.2f MB/s ===\n",
		total_ok, n, total_in, total_out, total_secs,
		total_secs > 0 ? (total_in / 1e6) / total_secs : 0,
		total_secs > 0 ? (total_out / 1e6) / total_secs : 0);

	/* ---- SD/mass-storage corpus: the actual deployment path (onboard TF
	 * card slot, not flash). Also re-tests every synthetic seed CHD -
	 * including hd_zlib.chd, the known-failing Class B repro - read through
	 * the real FATFS/SDMMC stack instead of a flash-mapped const array, to
	 * separate "read source" from "CPU execution" as candidate causes. ---- */
	printf("\n--- SD card corpus ---\n");
	sdmmc_card_t *card = mount_sdcard();
	if (!card) {
		printf("=== SD: no card mounted, skipping SD corpus ===\n");
		return;
	}

	static char *sd_paths[SD_MAX_FILES];
	int sd_n;
#if SD_USE_SAMPLE
	sd_n = 0;
	for (size_t si = 0; si < ARRAY_LEN(g_sd_sample) && sd_n < SD_MAX_FILES; si++) {
		struct stat st;
		if (stat(g_sd_sample[si], &st) != 0) {
			printf("SAMPLE MISSING: %s\n", g_sd_sample[si]);
			continue;
		}
		sd_paths[sd_n++] = strdup(g_sd_sample[si]);
	}
	printf("SD: characterized sample, %d of %zu entries present\n", sd_n, ARRAY_LEN(g_sd_sample));
#else
	sd_n = sd_scan_dir(SD_MOUNT_POINT, sd_paths, 0, 0);
	printf("SD: found %d *.chd file(s) under %s\n", sd_n, SD_MOUNT_POINT);
#endif


	g_max_hunks = SD_MAX_HUNKS_PER_FILE;
	if (g_max_hunks)
		printf("SD: reading at most %" PRIu32 " hunks per file\n", g_max_hunks);

	uint64_t sd_total_in = 0, sd_total_out = 0, sd_total_us = 0;
	int sd_total_ok = 0;

	for (int i = 0; i < sd_n; i++) {
#if BENCH_FATFS_BACKEND
		FIL *f = fffile_open(sd_paths[i]);
		if (!f) {
			printf("%-56s f_open FAILED\n", sd_paths[i]);
			continue;
		}
#else
		FILE *f = fopen(sd_paths[i], "rb");
		if (!f) {
			printf("%-56s fopen FAILED\n", sd_paths[i]);
			continue;
		}
#endif
		bool heap_ok_before = heap_caps_check_integrity_all(true);
#if BENCH_FATFS_BACKEND
		run_result r = run_one(sd_paths[i], &fffile_callbacks, f, fffile_fsize(f));
#else
		run_result r = run_one(sd_paths[i], &sdfile_callbacks, f, sdfile_fsize(f));
#endif
		bool heap_ok_after = heap_caps_check_integrity_all(true);
		if (!heap_ok_before || !heap_ok_after) {
			printf("  ^^^ heap corruption detected: before=%d after=%d\n", heap_ok_before, heap_ok_after);
		}
		/* run_one() already closes the chd_file (and thus calls
		 * sdfile_fclose on f via chd_close's core_fclose) on both the
		 * success and failure paths, so f is already closed here. */
		if (r.ok) {
			sd_total_in += r.compressed_bytes;
			sd_total_out += r.decompressed_bytes;
			sd_total_us += r.elapsed_us;
			sd_total_ok++;
		}
	}

	double sd_total_secs = sd_total_us / 1e6;
	printf("=== SD: %d/%d files OK, %" PRIu64 " bytes in / %" PRIu64 " bytes out, "
		"%.2f s total, avg in=%.2f MB/s out=%.2f MB/s ===\n",
		sd_total_ok, sd_n, sd_total_in, sd_total_out, sd_total_secs,
		sd_total_secs > 0 ? (sd_total_in / 1e6) / sd_total_secs : 0,
		sd_total_secs > 0 ? (sd_total_out / 1e6) / sd_total_secs : 0);

#if PASS_B_ENABLE
	printf("\n--- Pass B: request granularity, amplification, random vs sequential ---\n");
	printf("libchdr reads whole hunks only (no sub-hunk API, no decoded-hunk cache),\n"
	       "so a sub-hunk request costs a full hunk decode. 'amp' is bytes decoded /\n"
	       "bytes the caller asked for.\n");
	for (int i = 0; i < sd_n; i++)
		pass_b_file(sd_paths[i]);
#endif

	for (int i = 0; i < sd_n; i++)
		free(sd_paths[i]);

	/* explicit end-of-run marker so the serial capture knows when to stop
	 * without guessing from the last section's heading */
	printf("\n=== BENCH COMPLETE ===\n");
}
