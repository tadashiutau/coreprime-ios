#include <stdint.h>
#include <stddef.h>
#include "font.h"
#define REG(r) ({uint64_t x; __asm__ volatile("mrs %0, " r:"=r"(x)); x;})
void *memcpy(void *d,const void *s,size_t n){unsigned char *a=d;const unsigned char*b=s;while(n--)*a++=*b++;return d;}
void *memset(void *d,int c,size_t n){unsigned char*a=d;while(n--)*a++=c;return d;}
static int eq(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static uint32_t be32(const void*p){const uint8_t*b=p;return (uint32_t)b[0]<<24|(uint32_t)b[1]<<16|(uint32_t)b[2]<<8|b[3];}
static void textcopy(char*d,const void*s,size_t n,size_t cap){if(n>=cap)n=cap-1;memcpy(d,s,n);d[n]=0;}
static int compat(const char*a,size_t n,const char*b){size_t i=0;while(i<n){if(eq(a+i,b))return 1;while(i<n&&a[i])i++;i++;}return 0;}
/* Stable little-endian record for retrieval through lk2nd's ramoops raw command. */
struct record {
 char magic[8]; uint32_t version,bytes;
 uint64_t stage,el,midr,mmfr0,mmfr1,pfr0,cntfrq,cntvct,sctlr,tcr,dtb,dtbsize;
 uint64_t fb,fbsize,width,height,stride,format,ramoops,ramoops_size,uart,mem_count;
 uint64_t memory[16];
 uint64_t exception_slot,esr,elr,far;
 uint64_t gicd,gicc,vtimer_intid,vtimer_flags,initrd_start,initrd_end;
 uint64_t reserved_count,reserved[32];
 uint64_t checksum;
};
static struct record rec;
static char logbuf[8192];
static unsigned loglen,row,col;
static int psci_method; /* 1=SMC, 2=HVC, 0=unknown */
static int cpu1_spin, cpu1_psci; static uint64_t cpu1_release;
/* lk2nd's spin-table: where cpu@N waits for its entry point (0 = not given) */
static uint64_t cpu_release[4];
/* per cpu@N: enable-method (1 psci, 2 spin-table, 3 other) + 16 if status is disabled, + 32 if the node was seen */
static uint64_t cpu_kind[4];
static int coreprime_board;
static uint64_t hash(const void*p,size_t n){const uint8_t*b=p;uint64_t h=14695981039346656037ULL;while(n--){h^=*b++;h*=1099511628211ULL;}return h;}
static void persist(void){
 rec.checksum=hash(&rec,offsetof(struct record,checksum));
 if(rec.ramoops&&rec.ramoops_size>=sizeof(rec)+4096+sizeof(logbuf)){
  volatile uint8_t*d=(void*)(uintptr_t)rec.ramoops;const uint8_t*s=(void*)&rec;
  for(size_t i=0;i<sizeof(rec);i++)d[i]=s[i];
  for(unsigned i=0;i<loglen;i++)d[4096+i]=logbuf[i];
  d[4096+loglen]=0;__asm__ volatile("dsb sy":::"memory");
 }
}
static void pixel(unsigned x,unsigned y,uint32_t c){
 if(x>=rec.width||y>=rec.height)return;
 uintptr_t p=rec.fb+y*rec.stride+x*rec.format;
 if(rec.format==2)*(volatile uint16_t*)p=(uint16_t)(((c>>19)&31)<<11|((c>>10)&63)<<5|((c>>3)&31));
 else if(rec.format==3){volatile uint8_t*d=(void*)p;d[0]=c>>16;d[1]=c>>8;d[2]=c;}
 else if(rec.format==4)*(volatile uint32_t*)p=c;
}
static void putc(char c){
 if(loglen<sizeof(logbuf)-1)logbuf[loglen++]=c;
 if(rec.uart){volatile uint32_t *u=(void*)(uintptr_t)rec.uart;unsigned count=100000;while((u[0x18/4]&32)&&--count){}u[0]=(unsigned char)c;}
 if(c=='\n'){row++;col=0;return;}
 if(c=='\r')return;
 if(rec.fb&&rec.format&&row*16+16<=rec.height){
  unsigned ch=(unsigned char)c;if(ch<32||ch>126)ch='?';
  for(unsigned y=0;y<14;y++)for(unsigned x=0;x<8;x++)pixel(col*8+x,row*16+y,(font[ch-32][y]&(1u<<x))?0x8de6f0:0x10202b);
  if(++col>=rec.width/8){col=0;row++;}
 }
}
static void puts(const char*s){while(*s)putc(*s++);}
static void hex(uint64_t x){puts("0x");for(int i=60;i>=0;i-=4)putc("0123456789abcdef"[(x>>i)&15]);}
static void field(const char*s,uint64_t x){puts(s);puts(" = ");hex(x);putc('\n');}
struct node {
 char name[64],compatible[192],format[32],method[16],type[24],status[16],enable[24];
 uint64_t release_addr;
 uint32_t ac,sc,parent_ac,parent_sc,width,height,stride;
 uint8_t reg[128],interrupts[64];uint32_t reglen,interrupts_len;
 int reserved_child;
};
static struct node nodes[32];
static uint64_t cells(const uint8_t*p,unsigned n){uint64_t x=0;for(unsigned i=0;i<n;i++)x=x<<32|be32(p+4*i);return x;}
static void finish(struct node*n){
 unsigned ac=n->parent_ac,sc=n->parent_sc;
 /* how lk2nd says the second core is started: psci, or its own spin-table with a release address */
 if(eq(n->type,"cpu")&&eq(n->name,"cpu@1")){cpu1_spin=eq(n->enable,"spin-table");cpu1_psci=eq(n->enable,"psci");cpu1_release=n->release_addr;}
 if(n->name[0]=='c'&&n->name[1]=='p'&&n->name[2]=='u'&&n->name[3]=='@'&&n->name[4]>='0'&&n->name[4]<='3'&&n->name[5]==0)
  cpu_kind[n->name[4]-'0']=32|(eq(n->status,"disabled")?16:0)|(eq(n->enable,"psci")?1:eq(n->enable,"spin-table")?2:n->enable[0]?3:0);
 if(eq(n->type,"cpu")&&eq(n->enable,"spin-table")&&n->name[0]=='c'&&n->name[3]=='@'&&n->name[4]>='0'&&n->name[4]<='3'&&n->name[5]==0)cpu_release[n->name[4]-'0']=n->release_addr;
 if(ac<1||ac>2||sc<1||sc>2||eq(n->status,"disabled"))return;
 unsigned step=4*(ac+sc);
 if(eq(n->type,"memory"))for(unsigned p=0;p+step<=n->reglen&&rec.mem_count<8;p+=step){
  rec.memory[2*rec.mem_count]=cells(n->reg+p,ac);rec.memory[2*rec.mem_count+1]=cells(n->reg+p+4*ac,sc);rec.mem_count++;
 }
 if(compat(n->compatible,sizeof(n->compatible),"samsung,rossa"))coreprime_board=1;
 if(compat(n->compatible,sizeof(n->compatible),"arm,psci")||compat(n->compatible,sizeof(n->compatible),"arm,psci-0.2")||compat(n->compatible,sizeof(n->compatible),"arm,psci-1.0"))psci_method=eq(n->method,"hvc")?2:eq(n->method,"smc")?1:0;
 if((compat(n->compatible,sizeof(n->compatible),"arm,armv8-timer")||compat(n->compatible,sizeof(n->compatible),"arm,armv7-timer"))&&n->interrupts_len>=36){
  /* Standard binding: secure physical, nonsecure physical, virtual, hyp. */
  uint32_t type=be32(n->interrupts+24),index=be32(n->interrupts+28);
  if(type==1&&index<16){rec.vtimer_intid=index+16;rec.vtimer_flags=be32(n->interrupts+32);}
 }
 if(n->reglen<step)return;
 if(n->reserved_child)for(unsigned p=0;p+step<=n->reglen&&rec.reserved_count<16;p+=step){
  uint64_t base=cells(n->reg+p,ac),length=cells(n->reg+p+4*ac,sc);
  if(length){rec.reserved[2*rec.reserved_count]=base;rec.reserved[2*rec.reserved_count+1]=length;rec.reserved_count++;}
 }
 if((compat(n->compatible,sizeof(n->compatible),"qcom,msm-qgic2")||compat(n->compatible,sizeof(n->compatible),"arm,cortex-a15-gic")||compat(n->compatible,sizeof(n->compatible),"arm,gic-400"))&&n->reglen>=2*step){
  rec.gicd=cells(n->reg,ac);rec.gicc=cells(n->reg+step,ac);
 }
 uint64_t address=cells(n->reg,ac),size=cells(n->reg+4*ac,sc);
 if(compat(n->compatible,sizeof(n->compatible),"ramoops")){rec.ramoops=address;rec.ramoops_size=size;}
 if(compat(n->compatible,sizeof(n->compatible),"arm,pl011"))rec.uart=address;
 if(compat(n->compatible,sizeof(n->compatible),"simple-framebuffer")){
  unsigned bpp=eq(n->format,"r5g6b5")?2:eq(n->format,"r8g8b8")?3:(eq(n->format,"x8r8g8b8")||eq(n->format,"a8r8g8b8"))?4:0;
  if(address&&bpp&&n->width>=64&&n->width<=4096&&n->height<=4096&&n->height>=64&&n->stride>=(uint64_t)n->width*bpp&&size>=(uint64_t)n->height*n->stride){rec.fb=address;rec.fbsize=size;rec.width=n->width;rec.height=n->height;rec.stride=n->stride;rec.format=bpp;}
 }
}
static int parse(uintptr_t dtb){
 if(!dtb||be32((void*)dtb)!=0xd00dfeed)return 0;
 uint32_t total=be32((void*)(dtb+4)),so=be32((void*)(dtb+8)),st=be32((void*)(dtb+12));
 if(total<40||total>4*1024*1024||so>=total||st>=total)return 0;
 rec.dtbsize=total;uintptr_t p=dtb+so,end=dtb+total,strings=dtb+st;int depth=-1;
 while(p+4<=end){uint32_t token=be32((void*)p);p+=4;
  if(token==1){
   if(++depth>=32)return 0;struct node*n=&nodes[depth];memset(n,0,sizeof(*n));n->ac=2;n->sc=1;
   n->reserved_child=depth&&eq(nodes[depth-1].name,"reserved-memory");
   n->parent_ac=depth?nodes[depth-1].ac:2;n->parent_sc=depth?nodes[depth-1].sc:1;
   uintptr_t start=p;while(p<end&&*(const char*)p)p++;if(p>=end)return 0;
   textcopy(n->name,(void*)start,p-start,sizeof(n->name));p=(p+4)&~3ULL;
  }else if(token==2){if(depth<0)return 0;finish(&nodes[depth--]);}
  else if(token==3){
   if(depth<0||p+8>end)return 0;uint32_t len=be32((void*)p),off=be32((void*)(p+4));p+=8;
   if(len>end-p||off>=total-st)return 0;const char*name=(void*)(strings+off);struct node*n=&nodes[depth];
   if(eq(name,"#address-cells")&&len==4)n->ac=be32((void*)p);
   else if(eq(name,"#size-cells")&&len==4)n->sc=be32((void*)p);
   else if(eq(name,"reg")){n->reglen=len>sizeof(n->reg)?sizeof(n->reg):len;memcpy(n->reg,(void*)p,n->reglen);}
   else if(eq(name,"interrupts")){n->interrupts_len=len>sizeof(n->interrupts)?sizeof(n->interrupts):len;memcpy(n->interrupts,(void*)p,n->interrupts_len);}
   else if(eq(n->name,"chosen")&&eq(name,"linux,initrd-start")&&(len==4||len==8))rec.initrd_start=cells((void*)p,len/4);
   else if(eq(n->name,"chosen")&&eq(name,"linux,initrd-end")&&(len==4||len==8))rec.initrd_end=cells((void*)p,len/4);
   else if(eq(name,"compatible"))textcopy(n->compatible,(void*)p,len,sizeof(n->compatible));
   else if(eq(name,"format"))textcopy(n->format,(void*)p,len,sizeof(n->format));
   else if(eq(name,"method"))textcopy(n->method,(void*)p,len,sizeof(n->method));
   else if(eq(name,"enable-method"))textcopy(n->enable,(void*)p,len,sizeof(n->enable));
   else if(eq(name,"cpu-release-addr")&&(len==4||len==8))n->release_addr=cells((void*)p,len/4);
   else if(eq(name,"device_type"))textcopy(n->type,(void*)p,len,sizeof(n->type));
   else if(eq(name,"status"))textcopy(n->status,(void*)p,len,sizeof(n->status));
   else if(eq(name,"width")&&len==4)n->width=be32((void*)p);
   else if(eq(name,"height")&&len==4)n->height=be32((void*)p);
   else if(eq(name,"stride")&&len==4)n->stride=be32((void*)p);
   p=(p+len+3)&~3ULL;
  }else if(token==9)return depth==-1;
  else if(token!=4)return 0;
 }
 return 0;
}
__attribute__((noreturn)) static void halt(void){for(;;)__asm__ volatile("wfe");}
void diag_exception(uint64_t slot,uint64_t esr,uint64_t elr,uint64_t far){rec.stage=255;rec.exception_slot=slot;rec.esr=esr;rec.elr=elr;rec.far=far;persist();puts("EXCEPTION\n");field("ESR",esr);field("ELR",elr);field("FAR",far);persist();halt();}
static void reset(void){
 puts("RESETTING TO BOOT LOADER\n");persist();
 register uint64_t x0 __asm__("x0")=0x84000009;
 if(psci_method==2)__asm__ volatile("hvc #0":"+r"(x0)::"x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
 else if(psci_method==1)__asm__ volatile("smc #0":"+r"(x0)::"x1","x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
 /* Only this verified board's PS_HOLD register; never use the phone address in QEMU. */
 if(coreprime_board){*(volatile uint32_t*)0x004ab000=0;__asm__ volatile("dsb sy":::"memory");}
 halt();
}
void diag_main(uintptr_t dtb){
 memcpy(rec.magic,"CPDIAG64",8);rec.version=2;rec.bytes=sizeof(rec);rec.stage=1;
 rec.el=REG("CurrentEL");rec.midr=REG("MIDR_EL1");rec.mmfr0=REG("ID_AA64MMFR0_EL1");rec.mmfr1=REG("ID_AA64MMFR1_EL1");rec.pfr0=REG("ID_AA64PFR0_EL1");
 rec.cntfrq=REG("CNTFRQ_EL0");rec.cntvct=REG("CNTVCT_EL0");rec.sctlr=REG("SCTLR_EL1");rec.tcr=REG("TCR_EL1");rec.dtb=dtb;
 int valid=parse(dtb);rec.stage=valid?2:3;
 if(rec.fb)for(unsigned y=0;y<rec.height;y++)for(unsigned x=0;x<rec.width;x++)pixel(x,y,0x10202b);
 puts("CORE PRIME ARM64 DIAGNOSTIC v2\n\n");field("FDT VALID",valid);field("CurrentEL",rec.el);field("MIDR",rec.midr);field("MMFR0",rec.mmfr0);field("MMFR1",rec.mmfr1);field("PFR0",rec.pfr0);field("CNTFRQ",rec.cntfrq);field("SCTLR",rec.sctlr);field("TCR",rec.tcr);field("DTB",rec.dtb);field("RAM LOG",rec.ramoops);field("FRAMEBUFFER",rec.fb);
 for(unsigned i=0;i<rec.mem_count;i++){field("RAM BASE",rec.memory[2*i]);field("RAM SIZE",rec.memory[2*i+1]);}
 field("GIC DISTRIBUTOR",rec.gicd);field("GIC CPU INTERFACE",rec.gicc);field("VIRTUAL TIMER INTID",rec.vtimer_intid);field("RESERVED RANGES",rec.reserved_count);
 rec.stage=4;puts("\nREADY: CPU ENTRY CHECK COMPLETE\n");puts("NO STORAGE WRITES. REBOOT IN 12 SECONDS.\n");persist();
 uint64_t start=REG("CNTVCT_EL0"),span=rec.cntfrq*12;
 if(!rec.cntfrq)halt();while(REG("CNTVCT_EL0")-start<span){}
 reset();
}
