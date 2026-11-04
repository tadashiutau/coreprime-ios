/*
 * Core Prime: software implementation of the AArch64 ARMv8.0 Cryptographic
 * Extension for EL0 code on a Cortex-A53 built without it (MSM8916 here
 * reports ID_AA64ISAR0_EL1 = 0x10000: AES, SHA1, SHA2 all 0).
 *
 * Apple's arm64 corecrypto executes AESE etc. unconditionally. The kernel
 * calls coreprime_crypto_emulate() from the undefined-instruction handler;
 * it computes the architectural result (ARM ARM pseudocode for each
 * instruction) on the thread's saved 128-bit V registers. The caller advances
 * the PC only when this returns 1. Anything not recognised returns 0 and the
 * normal SIGILL path is kept.
 *
 * Covered: AESE AESD AESMC AESIMC, SHA1C SHA1P SHA1M SHA1SU0 SHA1H SHA1SU1,
 * SHA256H SHA256H2 SHA256SU0 SHA256SU1, PMULL/PMULL2 (1Q <- 1D x 1D).
 *
 * Self-contained so the same file builds in XNU and in the host test.
 */
#ifndef COREPRIME_CRYPTO_EMUL_H
#define COREPRIME_CRYPTO_EMUL_H

#include <stdint.h>

typedef struct { uint8_t b[16]; } cp_v128;

static const uint8_t cp_aes_sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t cp_aes_inv_sbox[256] = {
	0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
	0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
	0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
	0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
	0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
	0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
	0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
	0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
	0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
	0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
	0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
	0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
	0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
	0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
	0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
	0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

static inline uint8_t
cp_xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static inline uint8_t
cp_gmul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;
	for (int i = 0; i < 8; i++) {
		if (b & 1) {
			p ^= a;
		}
		a = cp_xtime(a);
		b >>= 1;
	}
	return p;
}

/* AES state byte i is row i%4, column i/4 (matches the ARM ARM). */
static void
cp_aese(cp_v128 *d, const cp_v128 *n, int decrypt)
{
	uint8_t s[16], t[16];
	for (int i = 0; i < 16; i++) {
		s[i] = d->b[i] ^ n->b[i];
	}
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			int src = decrypt ? (c - r + 4) % 4 : (c + r) % 4;
			t[r + 4 * c] = s[r + 4 * src];
		}
	}
	for (int i = 0; i < 16; i++) {
		d->b[i] = decrypt ? cp_aes_inv_sbox[t[i]] : cp_aes_sbox[t[i]];
	}
}

static void
cp_aesmc(cp_v128 *d, const cp_v128 *n, int inverse)
{
	static const uint8_t fwd[4] = {2, 3, 1, 1}, inv[4] = {14, 11, 13, 9};
	const uint8_t *m = inverse ? inv : fwd;
	cp_v128 out;
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			uint8_t v = 0;
			for (int k = 0; k < 4; k++) {
				v ^= cp_gmul(n->b[k + 4 * c], m[(k - r + 4) % 4]);
			}
			out.b[r + 4 * c] = v;
		}
	}
	*d = out;
}

static inline uint32_t
cp_w(const cp_v128 *v, int e)
{
	return (uint32_t)v->b[4 * e] | ((uint32_t)v->b[4 * e + 1] << 8) |
	       ((uint32_t)v->b[4 * e + 2] << 16) | ((uint32_t)v->b[4 * e + 3] << 24);
}

static inline void
cp_setw(cp_v128 *v, int e, uint32_t x)
{
	v->b[4 * e] = (uint8_t)x;
	v->b[4 * e + 1] = (uint8_t)(x >> 8);
	v->b[4 * e + 2] = (uint8_t)(x >> 16);
	v->b[4 * e + 3] = (uint8_t)(x >> 24);
}

static inline uint64_t
cp_dw(const cp_v128 *v, int e)
{
	return (uint64_t)cp_w(v, 2 * e) | ((uint64_t)cp_w(v, 2 * e + 1) << 32);
}

static inline uint32_t cp_rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t cp_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t cp_choose(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
static inline uint32_t cp_major(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x | y) & z); }
static inline uint32_t cp_parity(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }

/* SHA1C/SHA1P/SHA1M: kind 0, 1, 2. */
static void
cp_sha1_hash(cp_v128 *d, const cp_v128 *n, const cp_v128 *m, int kind)
{
	uint32_t x[4], y = cp_w(n, 0);
	for (int i = 0; i < 4; i++) {
		x[i] = cp_w(d, i);
	}
	for (int e = 0; e < 4; e++) {
		uint32_t t = kind == 0 ? cp_choose(x[1], x[2], x[3]) :
		    kind == 1 ? cp_parity(x[1], x[2], x[3]) : cp_major(x[1], x[2], x[3]);
		y = y + cp_rol(x[0], 5) + t + cp_w(m, e);
		x[1] = cp_rol(x[1], 30);
		/* <Y, X> = ROL(Y : X, 32) */
		uint32_t top = x[3];
		x[3] = x[2]; x[2] = x[1]; x[1] = x[0]; x[0] = y;
		y = top;
	}
	for (int i = 0; i < 4; i++) {
		cp_setw(d, i, x[i]);
	}
}

static void
cp_sha256_hash(cp_v128 *d, const cp_v128 *xv, const cp_v128 *yv, const cp_v128 *w, int part1)
{
	uint32_t x[4], y[4];
	for (int i = 0; i < 4; i++) {
		x[i] = cp_w(xv, i);
		y[i] = cp_w(yv, i);
	}
	for (int e = 0; e < 4; e++) {
		uint32_t chs = cp_choose(y[0], y[1], y[2]);
		uint32_t maj = cp_major(x[0], x[1], x[2]);
		uint32_t s1 = cp_ror(y[0], 6) ^ cp_ror(y[0], 11) ^ cp_ror(y[0], 25);
		uint32_t s0 = cp_ror(x[0], 2) ^ cp_ror(x[0], 13) ^ cp_ror(x[0], 22);
		uint32_t t = y[3] + s1 + chs + cp_w(w, e);
		x[3] = t + x[3];
		y[3] = t + s0 + maj;
		/* <Y, X> = ROL(Y : X, 32) */
		uint32_t xt = x[3], yt = y[3];
		x[3] = x[2]; x[2] = x[1]; x[1] = x[0]; x[0] = yt;
		y[3] = y[2]; y[2] = y[1]; y[1] = y[0]; y[0] = xt;
	}
	for (int i = 0; i < 4; i++) {
		cp_setw(d, i, part1 ? x[i] : y[i]);
	}
}

static void
cp_pmull64(cp_v128 *d, uint64_t a, uint64_t b)
{
	uint64_t lo = 0, hi = 0;
	for (int i = 0; i < 64; i++) {
		if ((b >> i) & 1) {
			lo ^= a << i;
			hi ^= i ? a >> (64 - i) : 0;
		}
	}
	cp_setw(d, 0, (uint32_t)lo);
	cp_setw(d, 1, (uint32_t)(lo >> 32));
	cp_setw(d, 2, (uint32_t)hi);
	cp_setw(d, 3, (uint32_t)(hi >> 32));
}

/*
 * v: the 32 saved Q registers as 16-byte little-endian blobs.
 * Returns 1 when instr was a crypto instruction and v was updated.
 */
static int
coreprime_crypto_emulate(uint32_t instr, cp_v128 v[32])
{
	unsigned rd = instr & 31, rn = (instr >> 5) & 31, rm = (instr >> 16) & 31;
	cp_v128 r, a = v[rd], b = v[rn], c = v[rm];

	if ((instr & 0xffff0c00) == 0x4e280800) {          /* AES two-register */
		switch ((instr >> 12) & 0xf) {
		case 4: r = a; cp_aese(&r, &b, 0); break;      /* AESE */
		case 5: r = a; cp_aese(&r, &b, 1); break;      /* AESD */
		case 6: cp_aesmc(&r, &b, 0); break;            /* AESMC */
		case 7: cp_aesmc(&r, &b, 1); break;            /* AESIMC */
		default: return 0;
		}
	} else if ((instr & 0xffe08c00) == 0x5e000000) {   /* SHA three-register */
		switch ((instr >> 12) & 7) {
		case 0: case 1: case 2:                        /* SHA1C, SHA1P, SHA1M */
			r = a; cp_sha1_hash(&r, &b, &c, (instr >> 12) & 7);
			break;
		case 3:                                        /* SHA1SU0 */
			for (int e = 0; e < 4; e++) {
				uint32_t t = e < 2 ? cp_w(&a, e + 2) : cp_w(&b, e - 2);
				cp_setw(&r, e, t ^ cp_w(&a, e) ^ cp_w(&c, e));
			}
			break;
		case 4: cp_sha256_hash(&r, &a, &b, &c, 1); break;   /* SHA256H */
		case 5: cp_sha256_hash(&r, &b, &a, &c, 0); break;   /* SHA256H2 */
		case 6: {                                      /* SHA256SU1 */
			uint32_t t0[4] = {cp_w(&b, 1), cp_w(&b, 2), cp_w(&b, 3), cp_w(&c, 0)};
			uint32_t res[4], t1[2] = {cp_w(&c, 2), cp_w(&c, 3)};
			for (int e = 0; e < 4; e++) {
				uint32_t x = e < 2 ? t1[e] : res[e - 2];
				x = cp_ror(x, 17) ^ cp_ror(x, 19) ^ (x >> 10);
				res[e] = x + cp_w(&a, e) + t0[e];
			}
			for (int e = 0; e < 4; e++) {
				cp_setw(&r, e, res[e]);
			}
			break;
		}
		default: return 0;
		}
	} else if ((instr & 0xffff0c00) == 0x5e280800) {   /* SHA two-register */
		switch ((instr >> 12) & 0xf) {
		case 0:                                        /* SHA1H: scalar, rest zeroed */
			for (int i = 0; i < 16; i++) {
				r.b[i] = 0;
			}
			cp_setw(&r, 0, cp_rol(cp_w(&b, 0), 30));
			break;
		case 1: {                                      /* SHA1SU1 */
			uint32_t t[4];
			for (int e = 0; e < 4; e++) {
				t[e] = cp_w(&a, e) ^ (e < 3 ? cp_w(&b, e + 1) : 0);
			}
			for (int e = 0; e < 4; e++) {
				cp_setw(&r, e, cp_rol(t[e], 1) ^ (e == 3 ? cp_rol(t[0], 2) : 0));
			}
			break;
		}
		case 2:                                        /* SHA256SU0 */
			for (int e = 0; e < 4; e++) {
				uint32_t x = e < 3 ? cp_w(&a, e + 1) : cp_w(&b, 0);
				x = cp_ror(x, 7) ^ cp_ror(x, 18) ^ (x >> 3);
				cp_setw(&r, e, x + cp_w(&a, e));
			}
			break;
		default: return 0;
		}
	} else if ((instr & 0xbfe0fc00) == 0x0ee0e000) {   /* PMULL{2} Vd.1Q, Vn.1D, Vm.1D */
		int hi = (instr >> 30) & 1;
		cp_pmull64(&r, cp_dw(&b, hi), cp_dw(&c, hi));
	} else {
		return 0;
	}
	v[rd] = r;
	return 1;
}

#endif /* COREPRIME_CRYPTO_EMUL_H */
