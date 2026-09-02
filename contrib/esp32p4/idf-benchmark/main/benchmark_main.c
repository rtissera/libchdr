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

static size_t sdfile_fread(void *ptr, size_t size, size_t nmemb, void *argp)
{
	return fread(ptr, size, nmemb, (FILE *)argp);
}

static int sdfile_fclose(void *argp)
{
	return fclose((FILE *)argp);
}

static int sdfile_fseek(void *argp, int64_t offset, int whence)
{
	return fseek((FILE *)argp, (long)offset, whence);
}

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

/* Per-file hunk cap for the SD sweep (0 = read every hunk). A real 43GB ROM
 * set has single titles of 20k+ hunks; reading all of them uncapped is an
 * hours-long run. 600 hunks/file keeps a full 128-file sweep to minutes while
 * still decoding a representative slice of every title. */
#define SD_MAX_HUNKS_PER_FILE 600

static uint32_t g_max_hunks = 0;

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

	int bad = 0;
	uint32_t bad_hunk = 0;
	for (uint32_t i = 0; i < nhunks; i++) {
		memset(buf, 0xAA, header->hunkbytes); /* poison, so a no-op decode is visible */
		err = chd_read(chd, i, buf);
		if (err != CHDERR_NONE) { bad = 1; bad_hunk = i; break; }
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
	int sd_n = sd_scan_dir(SD_MOUNT_POINT, sd_paths, 0, 0);
	printf("SD: found %d *.chd file(s) under %s\n", sd_n, SD_MOUNT_POINT);

	g_max_hunks = SD_MAX_HUNKS_PER_FILE;
	if (g_max_hunks)
		printf("SD: reading at most %" PRIu32 " hunks per file\n", g_max_hunks);

	uint64_t sd_total_in = 0, sd_total_out = 0, sd_total_us = 0;
	int sd_total_ok = 0;

	for (int i = 0; i < sd_n; i++) {
		FILE *f = fopen(sd_paths[i], "rb");
		if (!f) {
			printf("%-56s fopen FAILED\n", sd_paths[i]);
			continue;
		}
		bool heap_ok_before = heap_caps_check_integrity_all(true);
		run_result r = run_one(sd_paths[i], &sdfile_callbacks, f, sdfile_fsize(f));
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
		free(sd_paths[i]);
	}

	double sd_total_secs = sd_total_us / 1e6;
	printf("=== SD: %d/%d files OK, %" PRIu64 " bytes in / %" PRIu64 " bytes out, "
		"%.2f s total, avg in=%.2f MB/s out=%.2f MB/s ===\n",
		sd_total_ok, sd_n, sd_total_in, sd_total_out, sd_total_secs,
		sd_total_secs > 0 ? (sd_total_in / 1e6) / sd_total_secs : 0,
		sd_total_secs > 0 ? (sd_total_out / 1e6) / sd_total_secs : 0);
}
