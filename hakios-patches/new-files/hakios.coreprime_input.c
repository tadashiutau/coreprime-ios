/*
 * Samsung Galaxy Core Prime (SM-G360T1, MSM8916): the touch screen and the GPIO buttons.
 *
 * Boot-arg coreprime_input=1. Everything is polled from one kernel thread; no interrupt is taken.
 *
 *   touch screen  Imagis IST30xx ("core3" firmware) at 0x50 on BLSP1 QUP5 (I2C, 0x078b9000),
 *                 pins GPIO 18 (SDA) / 19 (SCL) function 2, its supply switched by GPIO 75
 *                 (TSP_EN, 1 = on; LDO6, its 1.8 V I/O rail, is already on for the display), its
 *                 "report ready" line on GPIO 13 (low while a report waits).
 *   home          GPIO 109, low while pressed
 *   volume up     GPIO 107, low while pressed
 *
 * Sources: the stock device tree (analysis/dts in the user's Driver RE data) and Samsung's kernel for
 * this board (lineage-16.0/kernel/samsung/msm8916: drivers/i2c/busses/i2c-msm-v2.c for the QUP's
 * version-2 tags in FIFO mode, drivers/input/touchscreen/imagis_core3 for the controller's protocol,
 * drivers/clk/qcom/clock-gcc-8916.c for the clocks, drivers/spmi/spmi-pmic-arb.c for the PMIC).
 *
 *   power, volume down  the PM8916's PON block (SPMI slave 0, peripheral 0x08): INT_RT_STS 0x810, bit 0
 *                 KPDPWR (power key), bit 1 RESIN (volume down), 1 while pressed. Read through the PMIC
 *                 arbiter v2 (core 0x0200f000, observer 0x02c00000): the channel of peripheral 0x08 from
 *                 the core's table at 0x800 + 4 * n, then a READ command (opcode 15) on that channel.
 *
 * The I2C controller: QUP state machine RESET -> RUN -> PAUSE (fill the output FIFO with tags and data)
 * -> RUN, then wait for MAX_OUTPUT_DONE (and MAX_INPUT_DONE for a read). Tag bytes: START 0x81 + address
 * byte, DATA_WRITE 0x82 / DATA_WRITE_N_STOP 0x83 / DATA_READ_N_STOP 0x87 + length. A read's data comes
 * back after two bytes of tag. Clock: 19.2 MHz from XO, fs_div 124 / ht_div 62 = 100 kHz.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define TLMM_PHYS       0x01000000ULL
#define TLMM_SIZE       0x00080000ULL   /* 122 GPIOs, 0x1000 each */
#define GCC_PHYS        0x01800000ULL
#define GCC_SIZE        0x00080000ULL
#define QUP5_PHYS       0x078b9000ULL
#define SPMI_CORE_PHYS  0x0200f000ULL
#define SPMI_OBS_PHYS   0x02c00000ULL
#define SPMI_OBS_SIZE   0x00400000ULL

#define GPIO_CFG(n)     (0x1000u * (n))
#define GPIO_INOUT(n)   (0x1000u * (n) + 4)

#define GCC_BLSP1_AHB_VOTE      0x45004 /* APCS_CLOCK_BRANCH_ENA_VOTE, bit 10 */
#define GCC_QUP5_I2C_CBCR       0x06020

#define QUP_CONFIG              0x000
#define QUP_STATE               0x004
#define QUP_IO_MODES            0x008
#define QUP_SW_RESET            0x00c
#define QUP_OPERATIONAL         0x018
#define QUP_ERROR_FLAGS         0x01c
#define QUP_ERROR_FLAGS_EN      0x020
#define QUP_OPERATIONAL_MASK    0x028
#define QUP_HW_VERSION          0x030
#define QUP_MX_OUTPUT_COUNT     0x100
#define QUP_OUT_FIFO_BASE       0x110
#define QUP_MX_WRITE_COUNT      0x150
#define QUP_MX_INPUT_COUNT      0x200
#define QUP_MX_READ_COUNT       0x208
#define QUP_IN_FIFO_BASE        0x218
#define QUP_I2C_MASTER_CLK_CTL  0x400
#define QUP_I2C_STATUS          0x404
#define QUP_I2C_MASTER_CONFIG   0x408

#define QUP_STATE_RESET         0
#define QUP_STATE_RUN           1
#define QUP_STATE_PAUSE         3
#define QUP_STATE_VALID         (1u << 2)
#define QUP_MAX_OUTPUT_DONE     (1u << 10)
#define QUP_MAX_INPUT_DONE      (1u << 11)
#define QUP_I2C_ERRORS          0x38000fcu

#define IST_ADDR                0x50
#define IST_CMD_SET_NOISE_MODE  0x0a
#define IST_CMD_START_SCAN      0x0b
#define IST_CMD_GET_COORD       0x20
#define IST_CMD_GET_CHIP_ID     0x30
#define IST_CMD_GET_FW_VER      0x31
#define IST_CMD_GET_LCD_RES     0x33
#define IST_MAX_FINGERS         10

static vm_offset_t tlmm, gcc, qup, spmi_core, spmi_obs;
static int pon_chnl = -1;
static uint32_t touch_w = 480, touch_h = 800;   /* the controller's coordinate range (boot-args touch_w=, touch_h=) */
static uint32_t touch_log = 1;
/*
 * With fb_aspect=1 the iPhone's picture (375 x 812 points) fills the panel's height and is centred across
 * it (MiniIOMobileFramebuffer.cpp); a touch is placed in that picture, one on the bars at its edge.
 */
static uint32_t aspect_x0, aspect_w;            /* in touch coordinates; aspect_w 0 = the whole width */

static inline uint32_t rd(vm_offset_t base, uint32_t off) { return *(volatile uint32_t *)(base + off); }
static inline void wr(vm_offset_t base, uint32_t off, uint32_t v) { *(volatile uint32_t *)(base + off) = v; __builtin_arm_dmb(0xb); }

static void ms_sleep(uint32_t ms) { delay_for_interval(ms, NSEC_PER_MSEC); }

/* pull: 0 none, 1 down, 3 up; func 0 = GPIO; output: -1 input, else the level */
static void
gpio_setup(unsigned n, unsigned func, unsigned pull, int output)
{
	uint32_t cfg = (pull & 3) | ((func & 0xf) << 2) | (0 << 6) /* 2 mA */ | (output >= 0 ? (1u << 9) : 0);

	if (output >= 0) {
		wr(tlmm, GPIO_INOUT(n), output ? 2 : 0);
	}
	wr(tlmm, GPIO_CFG(n), cfg);
}

static inline int gpio_in(unsigned n) { return rd(tlmm, GPIO_INOUT(n)) & 1; }

static int
qup_wait_state(uint32_t want, bool any)
{
	for (int i = 0; i < 2000; i++) {
		uint32_t s = rd(qup, QUP_STATE);
		if ((s & QUP_STATE_VALID) && (any || (s & 3) == want)) {
			return 0;
		}
		delay(5);
	}
	return EIO;
}

static int
qup_set_state(uint32_t state)
{
	if (qup_wait_state(0, true)) {
		return EIO;
	}
	wr(qup, QUP_STATE, state);
	return qup_wait_state(state, false);
}

static int
qup_init(void)
{
	wr(qup, QUP_SW_RESET, 1);
	if (qup_wait_state(QUP_STATE_RESET, false)) {
		return EIO;
	}
	wr(qup, QUP_CONFIG, 0x7 | 0x200);               /* N = 8 bits, mini core = I2C */
	wr(qup, QUP_ERROR_FLAGS_EN, 0x3c);
	wr(qup, QUP_OPERATIONAL_MASK, (1u << 9) | (1u << 8));
	wr(qup, QUP_I2C_MASTER_CONFIG, 1);             /* version-2 tags */
	return 0;
}

/* One transaction: write wlen bytes, then (if rlen) a repeated start and read rlen bytes; stop at the end. */
static int
i2c_xfer(uint8_t addr, const uint8_t *wbuf, uint32_t wlen, uint8_t *rbuf, uint32_t rlen)
{
	uint8_t out[64];
	uint32_t n = 0, words[16], status = 0, op = 0;

	if (wlen > 32 || rlen > 48 || (wlen == 0 && rlen == 0)) {
		return EINVAL;
	}
	if (wlen) {
		out[n++] = 0x81; out[n++] = (uint8_t)(addr << 1);
		out[n++] = rlen ? 0x82 : 0x83; out[n++] = (uint8_t)wlen;
		for (uint32_t i = 0; i < wlen; i++) out[n++] = wbuf[i];
	}
	if (rlen) {
		out[n++] = 0x81; out[n++] = (uint8_t)((addr << 1) | 1);
		out[n++] = 0x87; out[n++] = (uint8_t)rlen;
	}
	if (qup_set_state(QUP_STATE_RESET)) return EIO;
	wr(qup, QUP_MX_INPUT_COUNT, 0);
	wr(qup, QUP_MX_OUTPUT_COUNT, 0);
	wr(qup, QUP_MX_READ_COUNT, rlen ? rlen + 2 : 0);
	wr(qup, QUP_MX_WRITE_COUNT, n);
	wr(qup, QUP_CONFIG, (rd(qup, QUP_CONFIG) & ~(0xc0u | 0x1fu | 0xf00u)) | (rlen ? 0 : 0x80u) | 0x7 | 0x200);
	wr(qup, QUP_IO_MODES, (rd(qup, QUP_IO_MODES) & ~((3u << 10) | (3u << 12) | (1u << 14) | (1u << 15) | (1u << 16))) |
	    (1u << 14) | (1u << 15));                  /* FIFO mode, pack and unpack */
	wr(qup, QUP_OPERATIONAL_MASK, 0);
	if (qup_set_state(QUP_STATE_RUN)) return EIO;
	wr(qup, QUP_I2C_MASTER_CLK_CTL, (62u << 16) | 124u);
	if (qup_set_state(QUP_STATE_PAUSE)) return EIO;
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = 0;
		for (uint32_t k = 0; k < 4 && i + k < n; k++) w |= (uint32_t)out[i + k] << (8 * k);
		wr(qup, QUP_OUT_FIFO_BASE, w);
	}
	if (qup_set_state(QUP_STATE_RUN)) return EIO;
	uint32_t want = QUP_MAX_OUTPUT_DONE | (rlen ? QUP_MAX_INPUT_DONE : 0);
	for (int i = 0; i < 4000; i++) {
		op = rd(qup, QUP_OPERATIONAL);
		status = rd(qup, QUP_I2C_STATUS);
		if ((status & QUP_I2C_ERRORS) || rd(qup, QUP_ERROR_FLAGS)) break;
		if ((op & want) == want) break;
		delay(5);
	}
	if ((status & QUP_I2C_ERRORS) || (op & want) != want) {
		uint32_t err = rd(qup, QUP_ERROR_FLAGS);
		wr(qup, QUP_ERROR_FLAGS, err);
		(void)qup_init();
		return (status & (1u << 3)) ? ENXIO : EIO;     /* bit 3: the address or a byte was not acknowledged */
	}
	if (rlen) {
		uint32_t total = rlen + 2, nw = (total + 3) / 4;
		for (uint32_t i = 0; i < nw; i++) words[i] = rd(qup, QUP_IN_FIFO_BASE);
		const uint8_t *in = (const uint8_t *)words;
		for (uint32_t i = 0; i < rlen; i++) rbuf[i] = in[2 + i];
	}
	wr(qup, QUP_OPERATIONAL, op & (QUP_MAX_OUTPUT_DONE | QUP_MAX_INPUT_DONE));
	(void)qup_set_state(QUP_STATE_RESET);
	return 0;
}

/* The PM8916's PON channel of the PMIC arbiter (-1 if it has none for us). */
static int
spmi_find_chnl(uint8_t sid, uint8_t pid)
{
	for (int c = 0; c < 128; c++) {
		uint32_t v = rd(spmi_core, 0x800 + 4 * c);
		if (v && ((v >> 16) & 0xf) == sid && ((v >> 8) & 0xff) == pid) return c;
	}
	return -1;
}

/* One byte of a PMIC register through the observer channel; -1 on failure. */
static int
spmi_read8(int chnl, uint16_t addr)
{
	vm_offset_t ch = spmi_obs + 0x8000u * (uint32_t)chnl;
	wr(ch, 0x00, (15u << 27) | ((addr & 0xffu) << 4) | 0);
	for (int i = 0; i < 200; i++) {
		uint32_t st = rd(ch, 0x08);
		if (st & 1) return (st & 0xe) ? -1 : (int)(rd(ch, 0x18) & 0xff);
		delay(1);
	}
	return -1;
}

/* The controller's 32-bit commands, big-endian on the wire. */
static int
ist_read(uint32_t cmd, uint32_t *out, uint32_t words)
{
	uint8_t w[4] = { (uint8_t)(cmd >> 24), (uint8_t)(cmd >> 16), (uint8_t)(cmd >> 8), (uint8_t)cmd };
	uint8_t r[48];
	int err = i2c_xfer(IST_ADDR, w, 4, r, 4 * words);

	for (uint32_t i = 0; !err && i < words; i++) {
		out[i] = (uint32_t)r[4 * i] << 24 | (uint32_t)r[4 * i + 1] << 16 | (uint32_t)r[4 * i + 2] << 8 | r[4 * i + 3];
	}
	return err;
}

static int
ist_write(uint32_t cmd, uint32_t val)
{
	uint8_t w[8] = { (uint8_t)(cmd >> 24), (uint8_t)(cmd >> 16), (uint8_t)(cmd >> 8), (uint8_t)cmd,
		         (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)val };
	return i2c_xfer(IST_ADDR, w, 8, NULL, 0);
}

static uint32_t
scale(uint32_t v, uint32_t range)
{
	if (range < 2) return 0;
	if (v >= range) v = range - 1;
	return (uint32_t)(((uint64_t)v * 65535u) / (range - 1));
}

static void
coreprime_input_thread(__unused void *arg, __unused wait_result_t wr_)
{
	uint32_t id = 0, ver = 0, res = 0, msg[1 + IST_MAX_FINGERS];
	uint32_t down = 0;                              /* a bit per finger id (1..10) that iOS has as down */
	uint32_t lastx[IST_MAX_FINGERS + 1] = { 0 }, lasty[IST_MAX_FINGERS + 1] = { 0 };
	int home_was = 1, volup_was = 1, touch_ok = 0, errors = 0, pon_was = 0;

	/* clocks: the BLSP1 bus interface (a vote) and QUP5's I2C core clock (19.2 MHz XO by default) */
	wr(gcc, GCC_BLSP1_AHB_VOTE, rd(gcc, GCC_BLSP1_AHB_VOTE) | (1u << 10));
	wr(gcc, GCC_QUP5_I2C_CBCR, rd(gcc, GCC_QUP5_I2C_CBCR) | 1);
	for (int i = 0; i < 100 && (rd(gcc, GCC_QUP5_I2C_CBCR) & (1u << 31)); i++) delay(10);
	printf("coreprime-input: QUP5 I2C clock %s (CBCR 0x%08x), QUP version 0x%08x\n",
	    (rd(gcc, GCC_QUP5_I2C_CBCR) & (1u << 31)) ? "still off" : "on", rd(gcc, GCC_QUP5_I2C_CBCR), rd(qup, QUP_HW_VERSION));

	gpio_setup(18, 2, 0, -1);                       /* SDA */
	gpio_setup(19, 2, 0, -1);                       /* SCL */
	gpio_setup(13, 0, 3, -1);                       /* touch report ready, pulled up */
	gpio_setup(109, 0, 3, -1);                      /* home */
	gpio_setup(107, 0, 3, -1);                      /* volume up */
	gpio_setup(75, 0, 0, 1);                        /* TSP_EN: the controller's supply on */
	ms_sleep(150);

	pon_chnl = spmi_find_chnl(0, 0x08);
	printf("coreprime-input: PMIC arbiter version 0x%08x, PON channel %d, PON status 0x%x\n",
	    rd(spmi_core, 0), pon_chnl, pon_chnl >= 0 ? spmi_read8(pon_chnl, 0x810) : -1);

	if (qup_init()) {
		printf("coreprime-input: the I2C controller does not reach its reset state\n");
	} else {
		int e1 = ist_read(IST_CMD_GET_CHIP_ID, &id, 1);
		int e2 = ist_read(IST_CMD_GET_FW_VER, &ver, 1);
		int e3 = ist_read(IST_CMD_GET_LCD_RES, &res, 1);
		printf("coreprime-input: touch controller chip id 0x%08x (err %d), firmware 0x%08x (err %d), resolution word 0x%08x (err %d)\n",
		    id, e1, ver, e2, res, e3);
		if (!e1 && id != 0 && id != 0xffffffff) {
			uint32_t rw = res >> 16, rh = res & 0xffff;
			if (!e3 && rw >= 200 && rw <= 2000 && rh >= 200 && rh <= 3000) {
				touch_w = rw; touch_h = rh;
			}
			PE_parse_boot_argn("touch_w", &touch_w, sizeof(touch_w));
			PE_parse_boot_argn("touch_h", &touch_h, sizeof(touch_h));
			(void)ist_write(IST_CMD_SET_NOISE_MODE, 12u << 16);
			(void)ist_write(IST_CMD_START_SCAN, 0);
			ms_sleep(100);
			touch_ok = 1;
			{
				uint32_t aspect = 0;
				if (PE_parse_boot_argn("fb_aspect", &aspect, sizeof(aspect)) && aspect) {
					aspect_w = (uint32_t) ((uint64_t) touch_h * 375 / 812);
					if (aspect_w >= touch_w) aspect_w = 0; else aspect_x0 = (touch_w - aspect_w) / 2;
				}
			}
			printf("coreprime-input: touch screen on, coordinates 0..%u x 0..%u, picture columns %u from %u\n",
			    touch_w - 1, touch_h - 1, aspect_w ? aspect_w : touch_w, aspect_x0);
		}
	}

	for (;;) {
		int home = gpio_in(109), volup = gpio_in(107);
		if (home != home_was) {
			hakios_touch_post(home ? HAKIOS_BUTTON_UP : HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_HOME, 0, 0);
			if (touch_log) printf("coreprime-input: home %s\n", home ? "up" : "down");
			home_was = home;
		}
		if (volup != volup_was) {
			hakios_touch_post(volup ? HAKIOS_BUTTON_UP : HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_VOLUME_UP, 0, 0);
			if (touch_log) printf("coreprime-input: volume up %s\n", volup ? "up" : "down");
			volup_was = volup;
		}
		if (pon_chnl >= 0) {
			int pon = spmi_read8(pon_chnl, 0x810);
			if (pon >= 0) {
				if ((pon ^ pon_was) & 1) {
					hakios_touch_post((pon & 1) ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, HAKIOS_BUTTON_LOCK, 0, 0);
					if (touch_log) printf("coreprime-input: power %s\n", (pon & 1) ? "down" : "up");
				}
				if ((pon ^ pon_was) & 2) {
					hakios_touch_post((pon & 2) ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, HAKIOS_BUTTON_VOLUME_DOWN, 0, 0);
					if (touch_log) printf("coreprime-input: volume down %s\n", (pon & 2) ? "down" : "up");
				}
				pon_was = pon & 3;
			}
		}
		if (touch_ok && gpio_in(13) == 0) {        /* the controller also reports a lift this way */
			uint32_t status = 0;
			if (ist_read(IST_CMD_GET_COORD, &status, 1) == 0 && (status & 0x00000c00u) == 0x00000c00u) {
				uint32_t count = (status >> 12) & 0xf, pressed = status & 0x3ff, now = 0;
				if (count > IST_MAX_FINGERS) count = 0;
				msg[0] = status;
				if (count && ist_read(IST_CMD_GET_COORD, &msg[1], count) != 0) count = 0;
				for (uint32_t i = 0; i < count; i++) {
					uint32_t f = msg[1 + i], fid = (f >> 28) & 0xf, x = (f >> 12) & 0xfff, y = f & 0xfff;
					if (fid == 0 || fid > IST_MAX_FINGERS || !(pressed & (1u << (fid - 1)))) continue;
					now |= 1u << fid;
					if (aspect_w) {
						uint32_t ax = x < aspect_x0 ? 0 : x - aspect_x0;
						lastx[fid] = scale(ax, aspect_w);
					} else {
						lastx[fid] = scale(x, touch_w);
					}
					lasty[fid] = scale(y, touch_h);
					hakios_touch_post((down & (1u << fid)) ? HAKIOS_TOUCH_MOVE : HAKIOS_TOUCH_DOWN, fid - 1, lastx[fid], lasty[fid]);
					if (touch_log && !(down & (1u << fid))) printf("coreprime-input: finger %u down at %u,%u\n", fid, x, y);
				}
				for (uint32_t fid = 1; fid <= IST_MAX_FINGERS; fid++) {
					if ((down & (1u << fid)) && !(now & (1u << fid))) {
						hakios_touch_post(HAKIOS_TOUCH_UP, fid - 1, lastx[fid], lasty[fid]);
						if (touch_log) printf("coreprime-input: finger %u up\n", fid);
					}
				}
				down = now;
			} else if (++errors <= 5) {
				printf("coreprime-input: touch report read failed (status 0x%08x)\n", status);
			}
		}
		ms_sleep(10);
	}
}

/*
 * Boot-arg coreprime_cam=1: a read-only look at what the front camera (SR200PC20) needs, as the
 * bootloader left it: its pins (MCLK1 GPIO 27, reset GPIO 28, standby GPIO 17, CCI0 GPIO 29/30), the
 * camera clocks in GCC (clock-gcc-8916.c), and the PM8916's LDO16 (cam_vio, 1.8 V; peripheral 0x4f).
 */
static void
coreprime_cam_look(void)
{
	static const struct { const char *name; uint32_t off; } clks[] = {
		{ "camss_top_ahb", 0x56004 }, { "camss_ahb", 0x5a014 }, { "cci", 0x51018 }, { "cci_ahb", 0x5101c },
		{ "mclk1", 0x53018 }, { "csi1", 0x4f03c }, { "csi1_ahb", 0x4f040 }, { "csi1phy", 0x4f048 },
		{ "csi1rdi", 0x4f050 }, { "csi1phytimer", 0x4f01c }, { "ispif_ahb", 0x50004 }, { "vfe0", 0x58038 },
		{ "vfe_ahb", 0x58044 }, { "vfe_axi", 0x58048 }, { "csi_vfe0", 0x58050 },
	};
	static const unsigned pins[] = { 27, 28, 17, 29, 30, 22, 23 };

	for (unsigned i = 0; i < sizeof(clks) / sizeof(clks[0]); i++) {
		uint32_t v = rd(gcc, clks[i].off);
		printf("coreprime-cam: clock %-14s CBCR 0x%08x (%s)\n", clks[i].name, v, (v & (1u << 31)) ? "off" : "on");
	}
	printf("coreprime-cam: mclk1 RCG cmd 0x%08x cfg 0x%08x\n", rd(gcc, 0x53000), rd(gcc, 0x53004));
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		printf("coreprime-cam: GPIO %u cfg 0x%03x in/out 0x%x\n", pins[i], rd(tlmm, GPIO_CFG(pins[i])) & 0x7ff, rd(tlmm, GPIO_INOUT(pins[i])) & 3);
	}
	int ch = spmi_find_chnl(0, 0x4f);
	if (ch < 0) {
		printf("coreprime-cam: LDO16 has no arbiter channel for us\n");
	} else {
		printf("coreprime-cam: LDO16 (channel %d): status1 0x%02x, enable 0x%02x, range 0x%02x, voltage 0x%02x\n",
		    ch, spmi_read8(ch, 0x4f08), spmi_read8(ch, 0x4f46), spmi_read8(ch, 0x4f40), spmi_read8(ch, 0x4f41));
	}
}

void
hakios_coreprime_input_init(void)
{
	uint32_t enabled = 0;
	DTEntry entry;
	const void *type;
	unsigned size;
	thread_t thread;

	if (!PE_parse_boot_argn("coreprime_input", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (SecureDTLookupEntry(NULL, "/arm-io", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "device_type", &type, &size) != kSuccess || size != 8 || memcmp(type, "msm8916", 8) != 0) {
		printf("coreprime-input: not an MSM8916 board\n");
		return;
	}
	PE_parse_boot_argn("touch_log", &touch_log, sizeof(touch_log));
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	qup = ml_io_map(QUP5_PHYS, 0x1000);
	spmi_core = ml_io_map(SPMI_CORE_PHYS, 0x1000);
	spmi_obs = ml_io_map(SPMI_OBS_PHYS, SPMI_OBS_SIZE);
	{
		uint32_t cam = 0;
		if (PE_parse_boot_argn("coreprime_cam", &cam, sizeof(cam)) && cam >= 1) {
			coreprime_cam_look();
		}
	}
	if (kernel_thread_start(coreprime_input_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
