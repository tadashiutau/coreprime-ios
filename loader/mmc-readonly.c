/* SM-G360T1 retained-state SDHCI proof. No media-write/erase/switch commands.
 * Addresses/RCA/handoff from local lk2nd msm8916 source. Only reads LBA 0.
 * Not a block driver: relies on bootloader power/clocks/bus-width/tuning. */
static uint64_t mmc_hc=0x07824900ULL,mmc_core=0x07824000ULL;
#define MMC_HC mmc_hc
#define MMC_CORE mmc_core
static uint32_t mmcr(unsigned off) { return *(volatile uint32_t *)(uintptr_t)(MMC_HC+off); }
static void mmcw(unsigned off,uint32_t v) { *(volatile uint32_t *)(uintptr_t)(MMC_HC+off)=v; __asm__ volatile("dsb sy":::"memory"); }
static void mmcw16(unsigned off,uint16_t v) { *(volatile uint16_t *)(uintptr_t)(MMC_HC+off)=v; __asm__ volatile("dsb sy":::"memory"); }
static int mmcwait(unsigned off,uint32_t mask,uint32_t value) {
 uint64_t end=REG("CNTVCT_EL0")+rec.cntfrq;
 while((mmcr(off)&mask)!=value) if(REG("CNTVCT_EL0")>end)return 0;
 return 1;
}
static int mmccmd(unsigned cmd,uint32_t arg,unsigned flags) {
 /* Explicit allowlist: wake, select, status, single-block read only. */
 if(cmd!=5 && cmd!=7 && cmd!=13 && cmd!=17)return 0;
 field("MMC CMD",cmd);persist();
 if(!mmcwait(0x24,3,0)) { puts("MMC BUS INHIBITED\n"); return 0; }
 *(volatile uint8_t *)(uintptr_t)(MMC_HC+0x2e)=0xe; /* reset clears data timeout; LK programs this for every command */
 mmcw(0x30,0xffffffff);mmcw(8,arg);
 mmcw16(0xc,cmd==17?0x10:0);
 mmcw16(0xe,(cmd<<8)|flags);
 uint64_t end=REG("CNTVCT_EL0")+rec.cntfrq;
 uint32_t status;
 do {status=mmcr(0x30);if(REG("CNTVCT_EL0")>end)break;} while(!(status&0xffff8001));
 field("MMC IRQ",status);field("MMC RESP",mmcr(0x10));
 field("MMC COMMAND READBACK",mmcr(0x0c));field("MMC STATUS ENABLE",mmcr(0x34));field("MMC PRESENT AFTER",mmcr(0x24));persist();
 if((status&0xffff8000)||!(status&1))return 0;
 mmcw(0x30,1);
 if((flags&3)==3 && !mmcwait(0x24,2,0))return 0;
 return 1;
}
static void mmc_readonly_probe(void) {
 if(rec.gicd!=0x0b000000ULL || !pi.cmdline)return;
 int requested=0,sd=0;
 for(const char *c=pi.cmdline;*c;c++) {
  const char *key="mmc_probe=1";unsigned i=0;while(key[i]&&c[i]==key[i])i++;
  if(!key[i])requested=1;
  key="sd_probe=1";i=0;while(key[i]&&c[i]==key[i])i++;if(!key[i]){requested=1;sd=1;}
 }
 if(!requested)return;
 if(sd){mmc_hc=0x07864900ULL;mmc_core=0x07864000ULL;puts("MICROSD RETAINED-STATE PROBE\n");}
 puts("MMC READ-ONLY PROBE BEGIN\n");persist();
 volatile uint32_t *mode=(void *)(uintptr_t)(MMC_CORE+0x78);
 uint32_t original=*mode;field("MMC CORE HC MODE",original);
 field("MMC PRESENT",mmcr(0x24));field("MMC HOST POWER",mmcr(0x28));
 field("MMC CLOCK TIMEOUT",mmcr(0x2c));field("MMC CAPS",mmcr(0x40));
 field("MMC GCC APPS",*(volatile uint32_t *)(uintptr_t)0x01842018);
 field("MMC GCC AHB",*(volatile uint32_t *)(uintptr_t)0x0184201c);
 uint32_t saved_host=mmcr(0x28),saved_clock=mmcr(0x2c),saved_hc2=mmcr(0x3c);
 uint32_t saved_vendor=mmcr(0x10c),saved_dll=mmcr(0x100);
 persist();
 if(sd && (!(original&1) || !(saved_host&0x100) || !(saved_clock&4))) {
  puts("MICROSD NOT LEFT READY BY LK2ND; FULL INITIALIZATION REQUIRED\n");persist();return;
 }
 if(!sd) {
 *mode=0;
 volatile uint32_t *power=(void *)(uintptr_t)MMC_CORE;
 *power|=0x80;
 __asm__ volatile("dsb sy":::"memory");
 uint64_t core_end=REG("CNTVCT_EL0")+rec.cntfrq;
 while(*power&0x80)if(REG("CNTVCT_EL0")>core_end){puts("MMC CORE RESET TIMEOUT\n");*mode=original;persist();return;}
 *mode=original|1|(1u<<13); /* FF_CLK_SW_RST_DIS, as lk2nd sdhci_msm_init */__asm__ volatile("dsb sy":::"memory");
 field("MMC HC MODE READBACK",*mode);
 }
 uint32_t signals=mmcr(0x38),enable=mmcr(0x34);
 mmcw(0x38,0);mmcw(0x34,0xffffffff);
 field("MMC CORE VERSION",*(volatile uint32_t *)(uintptr_t)(MMC_CORE+0x50));
 field("MMC VENDOR",mmcr(0x10c));
 field("MMC ROOT CLOCK",*(volatile uint32_t *)(uintptr_t)0x01842004);
 field("MMC ROOT CONFIG",*(volatile uint32_t *)(uintptr_t)0x01842008);
 /* Reset host command/data engines; preserve power, bus width and card state. */
 *(volatile uint8_t *)(uintptr_t)(MMC_HC+0x2f)=6;
 __asm__ volatile("dsb sy":::"memory");
 if(!mmcwait(0x2c,0x06000000,0)){puts("MMC HOST RESET TIMEOUT\n");goto out;}
 field("MMC RESET COMPLETE",mmcr(0x2c));
 mmcw(0x28,saved_host);mmcw16(0x3e,saved_hc2>>16);
 mmcw(0x10c,saved_vendor);mmcw(0x100,saved_dll);
 mmcw16(0x2c,saved_clock&0xffff);
 mmcw16(0x2c,(uint16_t)mmcr(0x2c)|7);
 field("MMC CARD CLOCK RESTORED",mmcr(0x2c));
 /* lk2nd target_uninit: CMD7 deselect, CMD5 sleep, HC mode disabled. RCA=2. */
 if(!sd) { if(!mmccmd(5,2u<<16,0x1b)) { puts("MMC WAKE DID NOT COMPLETE; STOP\n");goto out; }
 if(!mmccmd(13,2u<<16,0x1a))goto out;
 if(!mmccmd(7,2u<<16,0x1b))goto out; }
 mmcw16(4,512);mmcw16(6,1);
 if(!mmccmd(17,0,0x3a))goto out;
 {
  uint64_t end=REG("CNTVCT_EL0")+rec.cntfrq;uint32_t status;
  do {status=mmcr(0x30);if(REG("CNTVCT_EL0")>end)break;}while(!(status&0xffff8020));
  field("MMC READ IRQ",status);
  if((status&0xffff8000)||!(status&0x20))goto out;
  uint8_t sector[512];
  for(unsigned i=0;i<128;i++){uint32_t v=mmcr(0x20);memcpy(sector+4*i,&v,4);}
  mmcw(0x30,0x20);
  if(!mmcwait(0x30,2,2)){puts("MMC TRANSFER NOT COMPLETE\n");goto out;}
  if(rec.ramoops && rec.ramoops_size>=0x40000) {
   uint8_t *d=(void *)(uintptr_t)(rec.ramoops+0x3000);
   memset(d,0,32);memcpy(d,"CPMMC001",8);((uint32_t*)d)[2]=512;
   memcpy(d+32,sector,512);clean_range((uintptr_t)d,544);
  }
  field("MMC LBA0 FNV",hash(sector,512));puts("MMC READ LBA0 COMPLETE\n");
 }
 out:
 mmcw(0x38,signals);mmcw(0x34,enable);*mode=original;
 puts("MMC READ-ONLY PROBE END\n");persist();
}
