/* Host known-answer test for coreprime_crypto_emul.h, driven by real encodings. */
#include <stdio.h>
#include <string.h>
#include "coreprime_crypto_emul.h"

static cp_v128 V[32];
static int failures;

static void
op(uint32_t base, int d, int n, int m)
{
	if (!coreprime_crypto_emulate(base | d | (n << 5) | (m << 16), V)) {
		printf("not decoded: %08x\n", base);
		failures++;
	}
}
#define AESE(d, n)      op(0x4e284800, d, n, 0)
#define AESD(d, n)      op(0x4e285800, d, n, 0)
#define AESMC(d, n)     op(0x4e286800, d, n, 0)
#define AESIMC(d, n)    op(0x4e287800, d, n, 0)
#define SHA1C(d, n, m)  op(0x5e000000, d, n, m)
#define SHA1P(d, n, m)  op(0x5e001000, d, n, m)
#define SHA1M(d, n, m)  op(0x5e002000, d, n, m)
#define SHA1SU0(d, n, m) op(0x5e003000, d, n, m)
#define SHA256H(d, n, m) op(0x5e004000, d, n, m)
#define SHA256H2(d, n, m) op(0x5e005000, d, n, m)
#define SHA256SU1(d, n, m) op(0x5e006000, d, n, m)
#define SHA1H(d, n)     op(0x5e280800, d, n, 0)
#define SHA1SU1(d, n)   op(0x5e281800, d, n, 0)
#define SHA256SU0(d, n) op(0x5e282800, d, n, 0)

static void
check(const char *name, const uint8_t *got, const uint8_t *want, int len)
{
	int ok = !memcmp(got, want, len);
	printf("%-28s %s\n", name, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
		for (int i = 0; i < len; i++) printf("%02x", got[i]);
		printf("\n");
	}
}

static void
addw(cp_v128 *d, const cp_v128 *a, const uint32_t *k)
{
	for (int e = 0; e < 4; e++) cp_setw(d, e, cp_w(a, e) + k[e]);
}

static void
load_be_words(cp_v128 *v, const uint8_t *p)
{
	for (int e = 0; e < 4; e++)
		cp_setw(v, e, (uint32_t)p[4 * e] << 24 | p[4 * e + 1] << 16 | p[4 * e + 2] << 8 | p[4 * e + 3]);
}

static void
test_aes(void)
{
	uint8_t key[16], pt[16], rk[11][16];
	static const uint8_t ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
		                       0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
	for (int i = 0; i < 16; i++) { key[i] = i; pt[i] = i * 0x11; }
	memcpy(rk[0], key, 16);
	uint8_t rcon = 1;
	for (int r = 1; r <= 10; r++) {
		uint8_t t[4] = {cp_aes_sbox[rk[r - 1][13]] ^ rcon, cp_aes_sbox[rk[r - 1][14]],
			        cp_aes_sbox[rk[r - 1][15]], cp_aes_sbox[rk[r - 1][12]]};
		rcon = cp_xtime(rcon);
		for (int i = 0; i < 16; i++) {
			rk[r][i] = rk[r - 1][i] ^ (i < 4 ? t[i] : rk[r][i - 4]);
		}
	}
	/* Encrypt in v7 using keys in v16..v26. */
	for (int r = 0; r <= 10; r++) memcpy(V[16 + r].b, rk[r], 16);
	memcpy(V[7].b, pt, 16);
	for (int r = 0; r < 9; r++) { AESE(7, 16 + r); AESMC(7, 7); }
	AESE(7, 25);
	for (int i = 0; i < 16; i++) V[7].b[i] ^= rk[10][i];
	check("AES-128 FIPS-197 encrypt", V[7].b, ct, 16);

	/* Equivalent inverse cipher: AESIMC on round keys 1..9. */
	for (int r = 1; r <= 9; r++) AESIMC(16 + r, 16 + r);
	AESD(7, 26); AESIMC(7, 7);
	for (int r = 9; r >= 2; r--) { AESD(7, 16 + r); AESIMC(7, 7); }
	AESD(7, 17);
	for (int i = 0; i < 16; i++) V[7].b[i] ^= rk[0][i];
	check("AES-128 FIPS-197 decrypt", V[7].b, pt, 16);
}

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/* Pads msg (len < 120) into 1 or 2 blocks; returns block count. */
static int
pad(uint8_t out[128], const char *msg)
{
	size_t len = strlen(msg);
	int blocks = len < 56 ? 1 : 2;
	memset(out, 0, 128);
	memcpy(out, msg, len);
	out[len] = 0x80;
	uint64_t bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) out[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
	return blocks;
}

static void
test_sha256(const char *name, const char *msg, const uint8_t want[32])
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint8_t buf[128], out[32];
	int blocks = pad(buf, msg);
	for (int blk = 0; blk < blocks; blk++) {
		/* v0 ABCD, v1 EFGH, v2 saved ABCD, v3 tmp, v4..v7 message */
		for (int e = 0; e < 4; e++) { cp_setw(&V[0], e, h[e]); cp_setw(&V[1], e, h[4 + e]); }
		for (int i = 0; i < 4; i++) load_be_words(&V[4 + i], buf + 64 * blk + 16 * i);
		for (int g = 0; g < 16; g++) {
			int m = 4 + (g % 4);
			addw(&V[3], &V[m], &K256[4 * g]);
			V[2] = V[0];
			SHA256H(0, 1, 3);
			SHA256H2(1, 2, 3);
			if (g < 12) {
				SHA256SU0(m, 4 + (g + 1) % 4);
				SHA256SU1(m, 4 + (g + 2) % 4, 4 + (g + 3) % 4);
			}
		}
		for (int e = 0; e < 4; e++) { h[e] += cp_w(&V[0], e); h[4 + e] += cp_w(&V[1], e); }
	}
	for (int i = 0; i < 8; i++)
		for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
	check(name, out, want, 32);
}

static void
test_sha1(const char *name, const char *msg, const uint8_t want[20])
{
	uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
	static const uint32_t K[4] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6};
	uint8_t buf[128], out[20];
	int blocks = pad(buf, msg);
	for (int blk = 0; blk < blocks; blk++) {
		/* v0 ABCD, v1 E (lane 0), v2 next E, v3 tmp, v4..v7 message */
		for (int e = 0; e < 4; e++) cp_setw(&V[0], e, h[e]);
		memset(&V[1], 0, 16);
		cp_setw(&V[1], 0, h[4]);
		for (int i = 0; i < 4; i++) load_be_words(&V[4 + i], buf + 64 * blk + 16 * i);
		for (int g = 0; g < 20; g++) {
			int m = 4 + (g % 4);
			uint32_t k[4] = {K[g / 5], K[g / 5], K[g / 5], K[g / 5]};
			addw(&V[3], &V[m], k);
			SHA1H(2, 0);
			switch (g / 5) {
			case 0: SHA1C(0, 1, 3); break;
			case 2: SHA1M(0, 1, 3); break;
			default: SHA1P(0, 1, 3); break;
			}
			V[1] = V[2];
			if (g < 16) {
				SHA1SU0(m, 4 + (g + 1) % 4, 4 + (g + 2) % 4);
				SHA1SU1(m, 4 + (g + 3) % 4);
			}
		}
		for (int e = 0; e < 4; e++) h[e] += cp_w(&V[0], e);
		h[4] += cp_w(&V[1], 0);
	}
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
	check(name, out, want, 20);
}

static void
test_pmull(void)
{
	uint64_t a = 0x87654321fedcba98ull, b = 0xf0e1d2c3b4a59687ull, c = 0x0123456789abcdefull;
	uint64_t lo = 0, hi = 0;
	/* independent reference: bitwise over 128-bit result */
	for (int i = 0; i < 64; i++)
		for (int j = 0; j < 64; j++)
			if (((a >> i) & 1) && ((c >> j) & 1)) {
				int k = i + j;
				if (k < 64) lo ^= 1ull << k; else hi ^= 1ull << (k - 64);
			}
	uint8_t want[16];
	for (int i = 0; i < 8; i++) { want[i] = (uint8_t)(lo >> (8 * i)); want[8 + i] = (uint8_t)(hi >> (8 * i)); }
	for (int i = 0; i < 8; i++) {
		V[9].b[i] = (uint8_t)(b >> (8 * i)); V[9].b[8 + i] = (uint8_t)(a >> (8 * i));
		V[10].b[i] = (uint8_t)(b >> (8 * i)); V[10].b[8 + i] = (uint8_t)(c >> (8 * i));
	}
	op(0x4ee0e000, 11, 9, 10);  /* PMULL2 v11.1q, v9.2d, v10.2d */
	check("PMULL2 vs reference", V[11].b, want, 16);
	/* PMULL low halves: b*b, compare with GF(2) square (bits spread) */
	uint64_t sl = 0, sh = 0;
	for (int i = 0; i < 64; i++)
		if ((b >> i) & 1) { int k = 2 * i; if (k < 64) sl |= 1ull << k; else sh |= 1ull << (k - 64); }
	for (int i = 0; i < 8; i++) { want[i] = (uint8_t)(sl >> (8 * i)); want[8 + i] = (uint8_t)(sh >> (8 * i)); }
	op(0x0ee0e000, 12, 9, 10);
	check("PMULL vs GF(2) square", V[12].b, want, 16);
	/* 8-bit PMULL (base NEON) must NOT be claimed */
	if (coreprime_crypto_emulate(0x0e20e000 | 12 | (9 << 5) | (10 << 16), V)) {
		printf("PMULL.8B wrongly decoded FAIL\n"); failures++;
	} else printf("%-28s ok\n", "PMULL.8B not claimed");
}

int
main(void)
{
	static const uint8_t s256_abc[32] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
	static const uint8_t s256_2blk[32] = {
		0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
		0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1};
	static const uint8_t s1_abc[20] = {
		0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
		0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
	static const uint8_t s1_2blk[20] = {
		0x84, 0x98, 0x3e, 0x44, 0x1c, 0x3b, 0xd2, 0x6e, 0xba, 0xae,
		0x4a, 0xa1, 0xf9, 0x51, 0x29, 0xe5, 0xe5, 0x46, 0x70, 0xf1};
	const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	test_aes();
	test_sha256("SHA-256 \"abc\"", "abc", s256_abc);
	test_sha256("SHA-256 two-block", two, s256_2blk);
	test_sha1("SHA-1 \"abc\"", "abc", s1_abc);
	test_sha1("SHA-1 two-block", two, s1_2blk);
	test_pmull();
	if (coreprime_crypto_emulate(0xd503201f, V)) { printf("NOP claimed FAIL\n"); failures++; }
	printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
	return failures != 0;
}
