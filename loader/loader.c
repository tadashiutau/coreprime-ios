/* Native lk2nd loader for the HakiOS XNU kernel on SM-G360T1 (msm8916).
 *
 * Replaces the port's UEFI loader: lk2nd hands us the patched device tree in
 * x0 and the XNUP payload rides in the boot image's ramdisk slot (exposed
 * through /chosen linux,initrd-start/end). The loader:
 *   - parses the lk2nd DTB (memory banks, reservations, framebuffer, ramoops,
 *     GIC, virtual timer INTID, PSCI conduit),
 *   - copies the kernel Mach-O segments into a validated free RAM window,
 *   - builds an Apple-format device tree and a boot_args rev 2 structure,
 *   - installs the kernel's return stub (XNULDRST) and enters the kernel.
 * Every log line goes to the framebuffer and to lk2nd's persistent ramoops
 * region; on exception, or when the kernel returns (loader stub), the loader
 * harvests the kernel's own log, persists everything and resets via PSCI so
 * `fastboot oem ramoops raw` retrieves it.
 */
#include <stdint.h>
#include <stddef.h>
#include "diag.c"	/* FDT walk, framebuffer text, ramoops persistence, PSCI */

/* ---------------- fixed conventions shared with the port ---------------- */

#define PAYLOAD_MAGIC       0x50554E58u	/* "XNUP" */
#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL
#define KERNEL_LINK_OFFSET  0x03000000ULL
#define BASE_ALIGN          0x02000000ULL	/* 32 MB */
#define MAX_MEM_SIZE        0x180000000ULL
#define PAGE_SIZE_MAX       0x4000ULL
#define DT_MAX              0x10000ULL
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
#define BOOT_LINE_LENGTH    608
#define KERNELC_LOG_CAP     0x24000u	/* kernel log bytes kept in ramoops */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t revision, version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};
#define MH_MAGIC_64   0xFEEDFACFu
#define LC_SEGMENT_64 0x19
#define LC_UNIXTHREAD 0x5

/* ---------------- loader state ---------------- */

uintptr_t loader_pa;	/* set by start.S: physical address of _start */
uintptr_t loader_sp_saved;	/* set by start.S: the loader's own stack, for the return entry */
extern void xnu_return_entry(void);	/* start.S: SP/MMU-safe entry the kernel is patched to */
static uintptr_t live_pa, log_cap_pa;	/* kernel loader_live, for the return stub */
static uint64_t the_slide;
static struct { uint64_t base, size; } win;

/* ---------------- Apple device tree writer (ported from xnuloader) ---------------- */

struct dt_writer {
	uint8_t *base, *cur, *end;
	int overflow;
	uint32_t scratch[2];
	uint32_t *counts[16];
	int depth;
};

static size_t str_len(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

static void dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	uint64_t padded = ((uint64_t)len + 3) & ~3ULL;
	if (w->overflow || str_len(name) > 32 || (uint64_t)(w->end - w->cur) < 36 + padded) {
		w->overflow = 1;
		return;
	}
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (size_t)padded);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + padded;
	w->counts[w->depth - 1][0]++;
}
static void dt_prop_str(struct dt_writer *w, const char *name, const char *v) { dt_prop(w, name, v, (uint32_t)str_len(v) + 1); }
static void dt_prop_u32(struct dt_writer *w, const char *name, uint32_t v) { dt_prop(w, name, &v, sizeof(v)); }
static void dt_prop_u64(struct dt_writer *w, const char *name, uint64_t v) { dt_prop(w, name, &v, sizeof(v)); }
static void dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];
	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}
static void dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) w->counts[w->depth - 1][1]++;
	if (w->overflow || (uint64_t)(w->end - w->cur) < 8) {
		w->overflow = 1;
		w->counts[w->depth++] = w->scratch;
		return;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}
static void dt_end(struct dt_writer *w) { w->depth--; }

/* Compatibility identity from the locally supplied iPhone X 16.5 IPSW.
 * These are userland metadata, not evidence that Apple peripherals exist. */
#include "identity-iphone-x.h"
static void dt_identity_props(struct dt_writer *w, const struct identity_prop *props,
                              const unsigned char *data, size_t count)
{
 for (size_t i = 0; i < count; i++)
  dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
}
#define dt_identity(w, kind) dt_identity_props(w, identity_##kind, identity_##kind##_data, sizeof(identity_##kind)/sizeof(identity_##kind[0]))

/* ---------------- NVRAM (ported from xnuloader) ---------------- */

static void nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;
	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	sum = h[0];
	for (int i = 2; i < 16; i++) sum += h[i];
	while (sum > 0xff) sum = (sum & 0xff) + (sum >> 8);
	h[1] = (uint8_t)sum;
}

static void build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;
	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
	memcpy(image + 20, &generation, 4);
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ---------------- free-window selection ---------------- */

/* One usable candidate block: [lo, hi) of a memory bank minus everything reserved. */
struct block { uint64_t lo, hi; };
static struct block blocks[16];
static int block_count;

static void cut(uint64_t lo, uint64_t hi)
{
	/* remove [lo,hi) from every collected block */
	for (int i = 0; i < block_count; i++) {
		if (blocks[i].lo < hi && blocks[i].hi > lo) {
			uint64_t a = lo > blocks[i].lo ? lo : blocks[i].lo;
			uint64_t b = hi < blocks[i].hi ? hi : blocks[i].hi;
			if (blocks[i].lo < a && b < blocks[i].hi && block_count < 16) {
				/* split: keep the tail here, add the head */
				blocks[block_count].lo = b;
				blocks[block_count].hi = blocks[i].hi;
				block_count++;
				blocks[i].hi = a;
			} else if (a <= blocks[i].lo) {
				blocks[i].lo = b;
			} else if (b >= blocks[i].hi) {
				blocks[i].hi = a;
			}
		}
	}
}

static void collect_blocks(void)
{
	for (unsigned i = 0; i < rec.mem_count && block_count < 16; i++) {
		uint64_t lo = rec.memory[2 * i], sz = rec.memory[2 * i + 1];
		if (sz == 0) continue;
		blocks[block_count].lo = lo;
		blocks[block_count].hi = lo + sz;
		block_count++;
	}
	/* reservations the DT declares */
	for (unsigned i = 0; i < rec.reserved_count; i++) {
		cut(rec.reserved[2 * i], rec.reserved[2 * i] + rec.reserved[2 * i + 1]);
	}
	/* the framebuffer, whether or not a reservation lists it */
	if (rec.fb) cut(rec.fb, rec.fb + rec.fbsize);
	/* the payload (initrd) and the DTB itself */
	if (rec.initrd_start && rec.initrd_end > rec.initrd_start) cut(rec.initrd_start, rec.initrd_end);
	if (rec.dtb) cut(rec.dtb, rec.dtb + rec.dtbsize);
	/* the loader image: header says the span; keep its BSS and stack too */
	cut(loader_pa, loader_pa + 0x00100000);
}

/* ---------------- PSCI and reset ---------------- */

static int64_t psci_call(uint64_t fid, uint64_t a0, uint64_t a1, uint64_t a2)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = a0;
	register uint64_t x2 __asm__("x2") = a1;
	register uint64_t x3 __asm__("x3") = a2;
	__asm__ volatile("dsb sy" ::: "memory");
	if (psci_method == 2)
		__asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "x4", "memory");
	else if (psci_method == 1)
		__asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "x4", "memory");
	else
		return -2;
	__asm__ volatile("dsb sy" ::: "memory");
	return (int64_t)x0;
}

static void clean_range(uint64_t pa, uint64_t len)
{
	uint64_t a = pa & ~63ULL, end = pa + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Our own holding pen for the secondary cores. lk2nd parks every core it starts in one shared
 * spin-table loop watching a single word (cpu@1's cpu-release-addr; it fails to describe the
 * others in the device tree). Writing that word would release them all at once, so the loader
 * moves them here instead, where each core watches its own slot (by MPIDR Aff0) and the kernel
 * releases them one at a time (psci_smp.c, spin-table path). The pen lives in the loader's
 * own megabyte, which the kernel never takes. MMU and caches are off for the cores in it.
 */
__attribute__((aligned(64))) volatile uint64_t cp_pen_slot[8];
extern char cp_pen[], cp_pen_end[];
__asm__(".text\n.balign 64\n.global cp_pen\ncp_pen:\n"
	"	mrs	x0, mpidr_el1\n"
	"	and	x0, x0, #3\n"
	"	adrp	x1, cp_pen_slot\n"
	"	add	x1, x1, :lo12:cp_pen_slot\n"
	"1:	wfe\n"
	"	ldr	x2, [x1, x0, lsl #3]\n"
	"	cbz	x2, 1b\n"
	"	br	x2\n"
	".global cp_pen_end\ncp_pen_end:\n");

/* Busy-wait on the generic timer (the loader has no other clock). */
static void loader_udelay(uint64_t us)
{
	uint64_t f = REG("CNTFRQ_EL0"), start = REG("CNTVCT_EL0");
	if (f == 0) f = 19200000ULL;
	while (REG("CNTVCT_EL0") - start < (f * us + 999999ULL) / 1000000ULL) { }
}

/*
 * Power a Cortex-A53 core of the MSM8916 up through its APCS ACC (the sequence Linux uses for
 * "qcom,msm8916-smp", arch/arm/mach-qcom/platsmp.c cortex_a7_release_secondary): reset and clamps
 * asserted, coremem clamp off, coremem GDHS closed, core clamp off, reset off, PWRDUP. The core
 * then starts through TrustZone at the cold-boot address lk2nd gave it (its spin-table pen).
 */
static void msm8916_power_up_core(int n)
{
	volatile uint32_t *acc = (volatile uint32_t *)(uintptr_t)(0x0b088000ULL + 0x10000ULL * (uint64_t)n);
	volatile uint32_t *pwr_ctl = acc + 0x04 / 4, *gate_ctl = acc + 0x14 / 4;
	*pwr_ctl = 0x00000033; __asm__ volatile("dsb sy" ::: "memory");
	*gate_ctl = 0x10000001; __asm__ volatile("dsb sy" ::: "memory"); loader_udelay(2);
	*pwr_ctl = 0x00000031; __asm__ volatile("dsb sy" ::: "memory");
	*pwr_ctl = 0x00000039; __asm__ volatile("dsb sy" ::: "memory"); loader_udelay(2);
	*pwr_ctl = 0x00020038; __asm__ volatile("dsb sy" ::: "memory"); loader_udelay(2);
	*pwr_ctl = 0x00020008; __asm__ volatile("dsb sy" ::: "memory");
	*pwr_ctl = 0x00020088; __asm__ volatile("dsb sy" ::: "memory");
}

/* Two full-resolution monochrome snapshots in the unused end of the ramoops
 * dump zone. Never touch lk2nd's console at +0x40000. Physical RGB framebuffer
 * is read before entry and after return, before the loader draws over it. */
static void snapshot_fb(unsigned offset)
{
 if (!rec.fb || rec.format != 4 || rec.width != 480 || rec.height != 800 ||
     !rec.ramoops || rec.ramoops_size < 0x40000) return;
 uint8_t *dst = (void *)(uintptr_t)(rec.ramoops + offset);
 memset(dst, 0, 32 + 48000);
 memcpy(dst, "CPFBMON1", 8);
 ((uint32_t *)dst)[2] = 480;
 ((uint32_t *)dst)[3] = 800;
 ((uint32_t *)dst)[4] = 48000;
 ((uint32_t *)dst)[5] = 1;
 uint64_t h = 14695981039346656037ULL;
 for (unsigned y = 0; y < 800; y++) {
  const volatile uint32_t *src = (const void *)(uintptr_t)(rec.fb + y * rec.stride);
  for (unsigned x = 0; x < 480; x++) {
   uint32_t v = src[x];
   for (unsigned k = 0; k < 4; k++) { h ^= (v >> (8*k)) & 255; h *= 1099511628211ULL; }
   unsigned lum = ((v >> 16) & 255)*77 + ((v >> 8) & 255)*150 + (v & 255)*29;
   if (lum >= 128*256) dst[32 + y*60 + x/8] |= 0x80 >> (x%8);
  }
 }
 ((uint64_t *)dst)[3] = h;
 clean_range((uintptr_t)dst, 32 + 48000);
}

/* ---------------- kernel return stub (MMU and caches are off here) ---------------- */

static void clean_by_pa(uint64_t pa, uint64_t len)
{
	uint64_t a = pa & ~63ULL, end = pa + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

__attribute__((noreturn)) void do_stub_persist_reset(void);

/* The kernel jumps here (XNULDRST slot 1) with MMU and caches off, after an
 * orderly end, a panic or loader_return_after. Harvest its log, persist, reset. */
__attribute__((noreturn, noinline, used)) void xnu_return_stub(
	uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	/* The kernel reaches here both with the MMU off (orderly return) and on
	 * (panic reboot on its own mappings, where these physical addresses are
	 * not mapped): drop the MMU and caches first, PC-relative code carries on.
	 * x2 is the reason (1 early exception, 2 checkpoint, 3 orderly return),
	 * x3 its code; the loader's log/ramoops pointers are all static-image
	 * kernel VAs, so the slide turns each into its physical address. */
	uint64_t sctlr;
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	if (sctlr & (1ULL | 0x4ULL | 0x1000ULL)) {
		sctlr &= ~(1ULL | 0x4ULL | 0x1000ULL);
		__asm__ volatile("msr sctlr_el1, %0" : : "r"(sctlr) : "memory");
		__asm__ volatile("isb\n ic iallu\n dsb sy\n isb" ::: "memory");
	}
	/* the reason came back with us: keep it in the record and the text log
	 * (stage 200+n = returned at checkpoint n) */
	if (a2 == 2 && a3 < 100) rec.stage = 200 + (uint32_t)a3;
	if (live_pa) {
		clean_by_pa(live_pa, 0x80);
		/* struct loader_live: {magic, heartbeat, log, log_len, ...} */
		const uint64_t *live = (const void *)live_pa;
		const uint8_t *log_va = (const uint8_t *)live[2];
		const uint32_t *len_p = (const uint32_t *)live[3];
		if (log_va && len_p) {
			uint64_t log_pa = (uint64_t)(uintptr_t)log_va + the_slide;
			uint64_t len_pa = (uint64_t)(uintptr_t)len_p + the_slide;
			clean_by_pa(len_pa, 8);
			clean_by_pa(log_pa, 0x1000);
			uint32_t len = *(const volatile uint32_t *)(uintptr_t)len_pa;
			if (len > 0x180000u) len = 0x180000u;	/* the kernel's LOADER_LOG_SIZE */
			if (rec.ramoops) {
				/*
				 * Only KERNELC_LOG_CAP fits. Drop launchd's own chatter (lines that begin
				 * "com.apple.xpc.launchd|"), then keep the start of what is left and as much of
				 * its end as fits -- the end has how the run finished and the kernel's message
				 * buffer. The filtering goes through the dead kernel's memory.
				 */
				static const char drop[] = "com.apple.xpc.launchd|", cut[] = "\n[... middle of the log left out ...]\n";
				uint8_t *dst = (void *)(uintptr_t)(rec.ramoops + 0x4000);
				uint8_t *tmp = (void *)(uintptr_t)(win.base + 0x10000000ULL);
				const volatile uint8_t *src = (const void *)log_pa;
				uint32_t o = 0;
				for (uint32_t i = 0; i < len;) {
					uint32_t k = 0;
					while (k < sizeof(drop) - 1 && i + k < len && src[i + k] == (uint8_t)drop[k]) k++;
					if (k == sizeof(drop) - 1) {
						/* ... except the lines that say how a process ended */
						static const char *const keep[] = { "exited", "signal", "crash", "kill", "jetsam", "abort" };
						uint32_t j = i, keepit = 0;
						while (j < len && src[j] != '\n') {
							for (unsigned w = 0; w < sizeof(keep) / sizeof(keep[0]) && !keepit; w++) {
								const char *t = keep[w]; uint32_t q = 0;
								while (t[q] && j + q < len && src[j + q] == (uint8_t)t[q]) q++;
								if (!t[q]) keepit = 1;
							}
							j++;
						}
						if (!keepit) { i = j + 1; continue; }
						k = 0;
					}
					while (i < len) { uint8_t c = src[i++]; tmp[o++] = c; if (c == '\n') break; }
				}
				if (o <= KERNELC_LOG_CAP) {
					for (uint32_t i = 0; i < o; i++) dst[i] = tmp[i];
				} else {
					uint32_t head = 0x8000u, mark = sizeof(cut) - 1, tail = KERNELC_LOG_CAP - head - mark - 1;
					for (uint32_t i = 0; i < head; i++) dst[i] = tmp[i];
					for (uint32_t i = 0; i < mark; i++) dst[head + i] = (uint8_t)cut[i];
					for (uint32_t i = 0; i < tail; i++) dst[head + mark + i] = tmp[o - tail + i];
					dst[KERNELC_LOG_CAP - 1] = '\n';
				}
			}
		}
	}
	snapshot_fb(0x34000);
	do_stub_persist_reset();
}

__attribute__((noreturn, used)) void do_stub_persist_reset(void)
{
	persist();
	puts("KERNEL RETURNED TO LOADER\n");
	persist();
	reset();
}

/* Exception entry from the vectors: record, report, reset so the log survives. */
void loader_exception(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	rec.stage = 255;
	rec.exception_slot = slot;
	rec.esr = esr;
	rec.elr = elr;
	rec.far = far;
	persist();
	puts("LOADER EXCEPTION\n");
	field("ESR", esr);
	field("ELR", elr);
	field("FAR", far);
	persist();
	reset();
}

/* ---------------- payload ---------------- */

static struct {
	const uint8_t *kernel; uint64_t kernel_size;
	const uint8_t *ramdisk; uint64_t ramdisk_size;
	const uint8_t *ramdisk1; uint64_t ramdisk1_size;	/* optional second disk, /dev/md1 */
	const char *cmdline, *kver;
	uint64_t checkpoint;
} pi;

static int parse_payload(uintptr_t pa)
{
	struct payload_header *h = (void *)pa;
	if (h->magic != PAYLOAD_MAGIC) return 0;
	uint8_t *p = (uint8_t *)(pa + 8), *pend = (uint8_t *)(pa + 8 + h->size);
	while (p + sizeof(struct payload_entry) <= pend) {
		struct payload_entry *e = (void *)p;
		if (e->name[0] == 0) break;
		uint8_t *data = p + sizeof(struct payload_entry);
		if ((uint64_t)e->size > (uint64_t)(pend - data)) return 0;
		if (str_eq(e->name, "kernel")) { pi.kernel = data; pi.kernel_size = e->size; }
		else if (str_eq(e->name, "ramdisk")) { pi.ramdisk = data; pi.ramdisk_size = e->size; }
		else if (str_eq(e->name, "ramdisk1")) { pi.ramdisk1 = data; pi.ramdisk1_size = e->size; }
		else if (str_eq(e->name, "cmdline")) pi.cmdline = (const char *)data;
		else if (str_eq(e->name, "kernel.ver")) pi.kver = (const char *)data;
		else if (str_eq(e->name, "checkpoint")) {
			/* decimal number in the payload's word; the kernel stops there
			 * (loader_checkpoint) and comes back through the stub */
			for (const char *c = (const char *)data; *c >= '0' && *c <= '9'; c++)
				pi.checkpoint = pi.checkpoint * 10 + (uint64_t)(*c - '0');
		}
		p = data + (e->size + 15 & ~15ULL);
	}
	return pi.kernel != 0;
}

/* ---------------- main ---------------- */

#include "mmc-readonly.c"

__attribute__((noreturn)) void loader_main(uintptr_t dtb)
{
	memcpy(rec.magic, "CPDIAG64", 8);
	rec.version = 2;
	rec.bytes = sizeof(rec);
	rec.cntfrq = REG("CNTFRQ_EL0");
	rec.cntvct = REG("CNTVCT_EL0");
	rec.stage = 1;
	int valid = parse(dtb);
	/* Clear only our dump zone; old kernel logs/snapshots must not survive a failed boot. */
	if (rec.ramoops && rec.ramoops_size >= 0x40000) {
		memset((void *)(uintptr_t)rec.ramoops, 0, 0x40000);
		clean_range(rec.ramoops, 0x40000);
	}
	if (rec.fb) for (unsigned y = 0; y < rec.height; y++) for (unsigned x = 0; x < rec.width; x++) pixel(x, y, 0x10202b);
	puts("CORE PRIME XNU LOADER (native, lk2nd)\n");
	field("FDT VALID", valid);
	field("LOADER PA", loader_pa);
	if (!valid) { persist(); puts("NO DEVICE TREE: HALT\n"); persist(); halt(); }
	if (!parse_payload(rec.initrd_start)) {
		puts("NO XNUP PAYLOAD AT INITRD\n");
		field("INITRD", rec.initrd_start);
		persist(); reset();
	}
	field("KERNEL", (uint64_t)(uintptr_t)pi.kernel);
	field("KERNEL SIZE", pi.kernel_size);
	field("RAMDISK SIZE", pi.ramdisk_size);
	field("RAMDISK1 SIZE", pi.ramdisk1_size);
	puts("CMDLINE: "); puts(pi.cmdline ? pi.cmdline : ""); putc('\n');

	mmc_readonly_probe();

	/* kernel Mach-O */
	const struct mach_header_64 *mh = (const void *)pi.kernel;
	if (mh->magic != MH_MAGIC_64) { puts("NOT A 64-BIT MACH-O\n"); persist(); reset(); }
	const struct load_command *lc = (const void *)(mh + 1);
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) continue;
			if (sg->vmaddr < vm_lo) vm_lo = sg->vmaddr;
			if (sg->vmaddr + sg->vmsize > vm_hi) vm_hi = sg->vmaddr + sg->vmsize;
		} else if (lc->cmd == LC_UNIXTHREAD && lc->cmdsize >= 16 + 8 * 33) {
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	field("KERNEL VA LO", vm_lo);
	field("KERNEL VA HI", vm_hi);
	field("KERNEL ENTRY", entry_va);
	if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || entry_va == 0 ||
	    vm_hi < vm_lo || vm_hi - KERNEL_VIRT_BASE > MAX_MEM_SIZE) {
		puts("UNEXPECTED KERNEL LAYOUT\n"); persist(); reset();
	}

	/* free window */
	collect_blocks();
	uint64_t best_lo = 0, best_sz = 0;
	for (int i = 0; i < block_count; i++) {
		uint64_t lo = (blocks[i].lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
		uint64_t hi = blocks[i].hi & ~(BASE_ALIGN - 1);
		if (hi <= lo) continue;
		/* the panic log lives above the window, inside the same block */
		uint64_t avail = hi - lo - PANIC_LOG_SIZE;
		if (avail > best_sz) { best_sz = avail; best_lo = lo; }
	}
	if (best_sz > MAX_MEM_SIZE) best_sz = MAX_MEM_SIZE;
	field("WINDOW", best_lo);
	field("WINDOW SIZE", best_sz);
	if (best_sz < 0x0C000000ULL) { puts("WINDOW TOO SMALL\n"); persist(); reset(); }
	win.base = best_lo;
	win.size = best_sz;

	/* layout */
	uint64_t phys_base = win.base;
	uint64_t slide = phys_base - KERNEL_VIRT_BASE;
	the_slide = slide;
	uint64_t kern_lo = vm_lo + slide;
	uint64_t kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t rd1_pa = rd_pa + rd_size;
	uint64_t rd1_size = (pi.ramdisk1_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	uint64_t dt_pa = rd1_pa + rd1_size;
	uint64_t ba_pa = dt_pa + DT_MAX;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;
	uint64_t pram_pa = win.base + win.size;	/* just above the window: PANIC_LOG_SIZE room was kept */
	if (win.size < top - phys_base + 0x4000000ULL) {
		puts("NOT ENOUGH CONTIGUOUS RAM\n"); persist(); reset();
	}
	field("PHYS BASE", phys_base);
	field("SLIDE", slide);
	field("TOP", top);

	/* copy segments */
	lc = (const void *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd != LC_SEGMENT_64) continue;
		const struct segment_command_64 *sg = (const void *)lc;
		if (sg->vmsize == 0) continue;
		uint64_t dst = sg->vmaddr + slide;
		if (sg->filesize > sg->vmsize || sg->fileoff + sg->filesize > pi.kernel_size ||
		    dst < phys_base || dst + sg->vmsize > top) {
			puts("SEGMENT OUT OF RANGE\n"); field("SEG", dst); persist(); reset();
		}
		memcpy((void *)dst, pi.kernel + sg->fileoff, sg->filesize);
		memset((void *)(dst + sg->filesize), 0, sg->vmsize - sg->filesize);
	}

	/* ramdisk */
	if (pi.ramdisk) memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
	if (pi.ramdisk1) memcpy((void *)rd1_pa, pi.ramdisk1, pi.ramdisk1_size);

	/* patch the kernel's return-stub slot ("XNULDRST") */
	{
		const uint8_t magic[8] = { 'X','N','U','L','D','R','S','T' };
		for (uint64_t off = 0; off + 32 <= pi.kernel_size; off += 8) {
			uint64_t k = 0;
			while (k < 8 && pi.kernel[off + k] == magic[k]) k++;
			if (k != 8) continue;
			/* find the segment containing this file offset */
			const struct load_command *l2 = (const void *)(mh + 1);
			uint64_t va = 0;
			for (uint32_t i = 0; i < mh->ncmds; i++, l2 = (const void *)((const uint8_t *)l2 + l2->cmdsize)) {
				if (l2->cmd != LC_SEGMENT_64) continue;
				const struct segment_command_64 *sg = (const void *)l2;
				if (off >= sg->fileoff && off + 24 <= sg->fileoff + sg->filesize) {
					va = sg->vmaddr + (off - sg->fileoff);
					break;
				}
			}
			if (!va) break;
			uint64_t slot_pa = va + slide;
			volatile uint64_t *slot = (void *)slot_pa;
			/* -fno-pic still resolves function addresses PC-relative (ADRP): the
			 * address taken at runtime is already the true physical address.
			 * xnu_return_entry first restores our stack and drops MMU/caches,
			 * because the kernel returns with a KVA stack pointer. */
			slot[1] = (uint64_t)(uintptr_t)&xnu_return_entry;
			slot[2] = pi.checkpoint;
			slot[3] = slide;
			/* the kernel's loader_live struct, for log harvest on return */
			const uint8_t lmagic[8] = { 'X','N','U','L','I','V','E','!' };
			for (uint64_t o2 = 0; o2 + 8 <= pi.kernel_size; o2 += 8) {
				uint64_t j = 0;
				while (j < 8 && pi.kernel[o2 + j] == lmagic[j]) j++;
				if (j == 8) {
					const struct load_command *l3 = (const void *)(mh + 1);
					for (uint32_t i = 0; i < mh->ncmds; i++, l3 = (const void *)((const uint8_t *)l3 + l3->cmdsize)) {
						if (l3->cmd != LC_SEGMENT_64) continue;
						const struct segment_command_64 *sg = (const void *)l3;
						if (o2 >= sg->fileoff && o2 + 8 <= sg->fileoff + sg->filesize) {
							live_pa = sg->vmaddr + (o2 - sg->fileoff) + slide;
							break;
						}
					}
					break;
				}
			}
			field("RETURN STUB", (uint64_t)(uintptr_t)&xnu_return_entry);
			field("LIVE STRUCT", live_pa);
			break;
		}
	}

	/* Apple device tree */
	uint64_t cntfrq = rec.cntfrq;
	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	uint8_t seed[64];
	for (unsigned i = 0; i < sizeof(seed); i++) seed[i] = (uint8_t)(REG("CNTVCT_EL0") >> (i & 7)) ^ (uint8_t)(i * 37);
	uint64_t io_base = rec.gicd & ~0x003fffffULL;
	if (io_base == 0) io_base = 0x00400000ULL;
	uint64_t io_size = 0x08000000ULL;
	uint64_t ranges[3] = { 0, io_base, io_size };
	uint64_t gic_reg[4] = { rec.gicd - io_base, 0x1000, rec.gicc - io_base, 0x2000 };
	uint64_t rd_map[2] = { rd_pa, rd_size };	/* the page-rounded size, as the port's loader reports */
	uint64_t rd1_map[2] = { rd1_pa, rd1_size };
	uint64_t pram_reg[2] = { pram_pa, PANIC_LOG_SIZE };
	uint64_t mpidr = REG("MPIDR_EL1") & 0x00ffffff;
	uint64_t cpu_freq[4] = { cntfrq, cntfrq, 996000000ULL, 100000000ULL };

	struct dt_writer w = { .base = (void *)dt_pa, .cur = (void *)dt_pa, .end = (void *)(dt_pa + DT_MAX), .depth = 0 };
	dt_begin(&w, "device-tree");
	dt_identity(&w, root);
	dt_prop_str(&w, "platform-name", "t8015");
	dt_prop_padded(&w, "serial-number", "RDW00POCOX5P", 32);
	dt_prop_padded(&w, "mlb-serial-number", "RDW00POCOX5PMLB00", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win.size);
	dt_prop_u32(&w, "debug-enabled", 1);
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	dt_prop_str(&w, "psci-method", psci_method == 2 ? "hvc" : "smc");
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	{
		static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
		static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
		static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
		static const uint8_t no_behavior[16];
		dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
		dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
		dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
		dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	}
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	/* /chosen/memory-map: where the kernel looks for the RAM disks */
	dt_begin(&w, "memory-map");
	dt_prop(&w, "RAMDisk", rd_map, sizeof(rd_map));
	if (rd1_size != 0) dt_prop(&w, "RAMDisk1", rd1_map, sizeof(rd1_map));	/* /dev/md1 */
	dt_end(&w);
	/* what the port's qemu platform scales machine timeouts by under TCG */
	if (rec.gicd == 0x08000000ULL) {
		dt_begin(&w, "machine-timeouts");
		dt_prop_u32(&w, "global-scale", 20);
		dt_end(&w);
	}
	dt_end(&w);
	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);
	dt_begin(&w, "product");
	dt_identity(&w, product);
	dt_begin(&w, "haptics");
	dt_identity(&w, haptics);
	dt_end(&w);
	dt_begin(&w, "camera");
	dt_identity(&w, camera);
	dt_end(&w);
	dt_end(&w);
	dt_begin(&w, "options");
	dt_end(&w);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);
	dt_begin(&w, "cpus");
	/*
	 * MSM8916: one cluster of four Cortex-A53, MPIDR Aff0 0..3. The kernel starts the others
	 * (osfmk/arm64/psci_smp.c) with PSCI CPU_ON or, for a node with cpu-release-addr, through lk2nd's
	 * spin-table, and only as many as boot-arg cpus= allows. The QEMU test board keeps one.
	 */
	/*
	 * Which of the four: all of them with PSCI; without it (this phone's stock TrustZone) the ones lk2nd
	 * parked in its spin-table (cpu-release-addr), which today is cpu@1 only.
	 */
	int cpu_count = 1, cpu_core[4] = { 0 };
	int pen = rec.gicd != 0x08000000ULL && (mpidr & 0xff) == 0 && psci_method == 0 && cpu1_release != 0;
	if (pen) {
		for (int n = 0; n < 8; n++) cp_pen_slot[n] = 0;
		clean_range((uint64_t)(uintptr_t)cp_pen, (uint64_t)(cp_pen_end - cp_pen));
		clean_range((uint64_t)(uintptr_t)cp_pen_slot, sizeof(cp_pen_slot));
		__asm__ volatile("ic iallu\n dsb sy\n isb" ::: "memory");
		*(volatile uint64_t *)(uintptr_t)cpu1_release = (uint64_t)(uintptr_t)cp_pen;
		clean_range(cpu1_release, 8);
		__asm__ volatile("sev" ::: "memory");
		field("PEN", (uint64_t)(uintptr_t)cp_pen);
		field("PEN SLOTS", (uint64_t)(uintptr_t)cp_pen_slot);
		/* lk2nd started cpu@1 only; the others are powered up here and find their way to the same pen */
		if (coreprime_board) {
			for (int n = 2; n < 4; n++) {
				volatile uint32_t *pwr_ctl = (volatile uint32_t *)(uintptr_t)(0x0b088004ULL + 0x10000ULL * (uint64_t)n);
				field(n == 2 ? "CPU2 ACC BEFORE" : "CPU3 ACC BEFORE", *pwr_ctl);
				if ((*pwr_ctl & 0x80) == 0) msm8916_power_up_core(n);
			}
		}
		for (int n = 1; n < 4; n++) cpu_core[cpu_count++] = n;
	} else if (rec.gicd != 0x08000000ULL && (mpidr & 0xff) == 0 && psci_method != 0) {
		for (int n = 1; n < 4; n++) cpu_core[cpu_count++] = n;
	}
	field("PSCI METHOD", (uint64_t)psci_method);
	field("BOOT MPIDR", mpidr);
	field("CPU COUNT", (uint64_t)cpu_count);
	field("CPU1 PSCI", (uint64_t)cpu1_psci);
	field("CPU1 SPIN TABLE", (uint64_t)cpu1_spin);
	field("CPU1 RELEASE ADDR", cpu1_release);
	field("CPU2 RELEASE ADDR", cpu_release[2]);
	field("CPU3 RELEASE ADDR", cpu_release[3]);
	field("CPU0 KIND", cpu_kind[0]);
	field("CPU1 KIND", cpu_kind[1]);
	field("CPU2 KIND", cpu_kind[2]);
	field("CPU3 KIND", cpu_kind[3]);
	for (int i = 0; i < cpu_count; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };
		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)(i == 0 ? mpidr : ((mpidr & ~0xffULL) | (uint64_t)cpu_core[i])));
		if (i != 0 && pen) dt_prop_u64(&w, "cpu-release-addr", (uint64_t)(uintptr_t)&cp_pen_slot[cpu_core[i]]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop(&w, "timebase-frequency", &cpu_freq[0], 8);
		dt_prop(&w, "fixed-frequency", &cpu_freq[1], 8);
		dt_prop(&w, "clock-frequency", &cpu_freq[2], 8);
		dt_prop(&w, "bus-frequency", &cpu_freq[3], 8);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		dt_end(&w);
	}
	dt_end(&w);
	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", rec.gicd == 0x08000000ULL ? "qemu-virt" : "msm8916");
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	dt_prop_str(&w, "compatible", "arm,cortex-a15-gic");
	dt_prop_u32(&w, "version", 2);
	if (rec.vtimer_intid) dt_prop_u32(&w, "virtual-timer-intid", (uint32_t)rec.vtimer_intid);
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);
	if (rec.uart) {
		uint64_t uart_reg[2] = { rec.uart - io_base, 0x1000 };
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		dt_prop(&w, "reg", uart_reg, sizeof(uart_reg));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);
	dt_end(&w);
	if (w.overflow) { puts("DEVICE TREE OVERFLOW\n"); persist(); reset(); }

	/* boot_args */
	struct boot_args *ba = (void *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win.size;
	ba->memSizeActual = win.size;
	ba->topOfKernelData = top;
	if (rec.fb) {
		ba->video.v_baseAddr = rec.fb;
		ba->video.v_display = 0; /* diagnostic text console, not graphical splash */
		ba->video.v_rowBytes = rec.stride;
		ba->video.v_width = rec.width;
		ba->video.v_height = rec.height;
		ba->video.v_depth = rec.format * 8;
	}
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = (uint32_t)(w.cur - w.base);
	{
		uint32_t n = 0;
		if (pi.kver) {
			static const char prefix[] = "hakios_kver=";
			for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) ba->commandLine[n++] = *c;
			for (const char *c = pi.kver; *c && *c != ' ' && n < BOOT_LINE_LENGTH - 1; c++) ba->commandLine[n++] = *c;
			if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
		}
		for (const char *c = pi.cmdline ? pi.cmdline : ""; *c && n < BOOT_LINE_LENGTH - 1; c++) ba->commandLine[n++] = *c;
	}
	field("DT", dt_pa);
	field("DT LEN", ba->deviceTreeLength);
	field("BOOT ARGS", ba_pa);
	field("V TIMER INTID", rec.vtimer_intid);
	memset((void *)pram_pa, 0, PANIC_LOG_SIZE);
	puts("JUMPING TO KERNEL\n");
	persist();
	snapshot_fb(0x28000);

	/* push everything the kernel will read out of dirty cache lines */
	clean_range(kern_lo, top - kern_lo);
	clean_range(loader_pa, 0x00100000);
	if (rec.ramoops) clean_range(rec.ramoops, 0x40000);

	/* clear the persistent-log tail: the kernel's log lands there on return */
	if (rec.ramoops) memset((void *)(rec.ramoops + 0x4000), 0, KERNELC_LOG_CAP);

	uint64_t entry = entry_va + slide;
	register uint64_t x0 __asm__("x0") = ba_pa;
	__asm__ volatile(
		"msr daifset, #0xf\n"
		"mov x2, sp\n"
		"sub x2, x2, #0x400\n"
		"bic x2, x2, #63\n"
		"add x3, x2, #0x4000\n"
		"1: dc cvac, x2\n"
		"add x2, x2, #64\n"
		"cmp x2, x3\n"
		"b.lo 1b\n"
		"dsb sy\n"
		"mrs x2, sctlr_el1\n"
		"bic x2, x2, #0x1\n"
		"bic x2, x2, #0x4\n"
		"bic x2, x2, #0x1000\n"
		"msr sctlr_el1, x2\n"
		"isb\n"
		"ic iallu\n"
		"dsb sy\n"
		"isb\n"
		"br %0\n"
		: : "r"(entry), "r"(x0) : "x2", "x3", "memory");
	__builtin_unreachable();
}
