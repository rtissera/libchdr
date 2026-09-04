/* Known-answer tests for the hand-optimised primitives.
 *
 * ecc_generate/ecc_verify/ecc_clear and crc16 were rewritten for speed - the
 * ECC P/Q parity now runs four rows at a time in a 32-bit word, and crc16 is
 * slice-by-4. Both are easy to get subtly wrong in ways nothing else notices:
 * a bad ECC byte still decodes, still passes the per-hunk CRC (which is
 * computed over the same wrong bytes), and only shows up as a corrupt sector
 * on real hardware. The awkward cases are the tails - 86 P rows is not a
 * multiple of 4, and crc16 must handle every length % 4 - plus mode 2's
 * special first byte.
 *
 * So pin the outputs. These constants were produced by the byte-at-a-time
 * implementations that predate those rewrites and verified equal to the
 * optimised ones over 300,000 randomised sectors and 4,217 crc16 lengths; if
 * a future change moves any of them, that change altered decoded output.
 *
 * Deliberately self-contained: a fixed xorshift generator rather than rand(),
 * which varies between C libraries, and byte-at-a-time hashing so the result
 * does not depend on host endianness or word size.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <libchdr/cdrom.h>

/* not exposed in a public header, but a global symbol in libchdr_chd.c */
extern uint16_t chd_crc16(const void *data, uint32_t length);

static uint64_t rng_state;

static void rng_seed(uint64_t s)
{
	rng_state = s;
}

static uint8_t rng_byte(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint8_t)(rng_state >> 24);
}

/* FNV-1a over bytes: endian- and width-independent */
static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	size_t i;
	for (i = 0; i < n; i++) {
		h ^= b[i];
		h *= 1099511628211ULL;
	}
	return h;
}

#define FNV_INIT 1469598103934665603ULL

/* Shape a deterministic sector, cycling through the payload patterns and
 * sector modes that drive the different ECC paths. */
static void make_sector(uint8_t *s, int i)
{
	int j;
	for (j = 0; j < CD_MAX_SECTOR_DATA; j++) {
		switch (i & 3) {
		case 0:  s[j] = rng_byte();      break;
		case 1:  s[j] = 0x00;            break;
		case 2:  s[j] = 0xFF;            break;
		default: s[j] = (uint8_t)(j * 7); break;
		}
	}
	/* sync pattern, then mode 1 / mode 2 form 1 / mode 2 form 2 */
	s[0] = 0x00;
	memset(s + 1, 0xFF, 10);
	s[11] = 0x00;
	s[15] = (i & 1) ? 1 : 2;
	if (s[15] == 2) {
		s[18] = (i & 2) ? 0x20 : 0x00;
		s[22] = s[18];
	}
}

static int check(const char *what, uint64_t got, uint64_t want)
{
	if (got == want) {
		printf("  ok    %-22s %016llx\n", what, (unsigned long long)got);
		return 0;
	}
	printf("  FAIL  %-22s got %016llx want %016llx\n", what,
	       (unsigned long long)got, (unsigned long long)want);
	return 1;
}

static int kat_ecc(void)
{
	uint8_t sector[CD_MAX_SECTOR_DATA];
	uint64_t hgen = FNV_INIT, hclr = FNV_INIT, hver = FNV_INIT;
	int i;

	rng_seed(88172645463325252ULL);
	for (i = 0; i < 2000; i++) {
		uint8_t v;

		make_sector(sector, i);
		ecc_generate(sector);
		hgen = fnv(hgen, sector, sizeof(sector));

		/* Pin what ecc_verify answers rather than asserting a semantic
		 * rule: mode 2 form 2 carries no ECC, so "generated sectors
		 * verify, corrupted ones do not" is not universally true, and a
		 * test that assumed it would encode a claim instead of the
		 * behaviour it is meant to freeze. */
		v = (uint8_t)(ecc_verify(sector) ? 1 : 0);
		hver = fnv(hver, &v, 1);

		sector[2076] ^= 0xFF;
		v = (uint8_t)(ecc_verify(sector) ? 1 : 0);
		hver = fnv(hver, &v, 1);
		sector[2076] ^= 0xFF;

		ecc_clear(sector);
		hclr = fnv(hclr, sector, sizeof(sector));
	}

	return check("ecc_generate", hgen, 0x6d5d06091a7c190dULL)
	     + check("ecc_verify",   hver, 0x9ff022cc3241f2c3ULL)
	     + check("ecc_clear",    hclr, 0xf283e958b1a5eb2dULL);
}

static int kat_crc16(void)
{
	static uint8_t buf[262144];
	/* the hunk geometries libchdr actually meets, from a 64-byte raw hunk
	 * to an AVHuff frame */
	static const uint32_t geo[] = {
		1, 2, 3, 5, 7, 12, 64, 96, 2352, 2448, 4096,
		9792, 19584, 219660, 223668
	};
	uint64_t h = FNV_INIT;
	uint32_t len;
	unsigned g;
	int off;
	size_t i;

	rng_seed(0x123456789abcdefULL);
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = rng_byte();

	/* every length 0..4096 covers all four slice-by-4 tail cases */
	for (len = 0; len <= 4096; len++) {
		uint16_t c = chd_crc16(buf, len);
		uint8_t be[2];
		be[0] = (uint8_t)(c >> 8);
		be[1] = (uint8_t)c;
		h = fnv(h, be, 2);
	}
	/* real geometries at several start offsets */
	for (g = 0; g < sizeof(geo) / sizeof(*geo); g++) {
		for (off = 0; off < 8; off++) {
			uint16_t c = chd_crc16(buf + off, geo[g]);
			uint8_t be[2];
			be[0] = (uint8_t)(c >> 8);
			be[1] = (uint8_t)c;
			h = fnv(h, be, 2);
		}
	}

	return check("crc16", h, 0xf49702c0d8af4f06ULL);
}

int main(void)
{
	int fail = 0;

	printf("libchdr known-answer tests\n");
	fail += kat_ecc();
	fail += kat_crc16();

	if (fail) {
		printf("%d known-answer test(s) FAILED - decoded output changed\n", fail);
		return 1;
	}
	printf("all known-answer tests passed\n");
	return 0;
}
