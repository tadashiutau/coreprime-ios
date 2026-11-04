/*
 * First user-space program for the Darwin-on-redwood bring-up.
 *
 * A static arm64 Mach-O with no libc and no dyld: it talks to the kernel with
 * raw system calls. The kernel's mockfs presents the ramdisk (this very file)
 * as /sbin/launchd, so this runs as process 1.
 */
typedef unsigned long u64;

/* XNU arm64 system call: number in x16, arguments in x0.., "svc #0x80"; carry set on error */
static long
syscall3(long num, long a, long b, long c)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16), "r" (x1), "r" (x2) : "memory", "cc");
	return x0;
}

/* the same with six arguments, for sysctlbyname(name, namelen, old, &oldlen, new, newlen) */
static long
syscall6(long num, long a, long b, long c, long d, long e, long f)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16), "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5) : "memory", "cc");
	return x0;
}

#define SYS_exit    1
#define SYS_read    3
#define SYS_mount   167
#define SYS_write   4
#define SYS_open    5
#define SYS_getpid  20
#define SYS_getuid  24
#define SYS_select  93
#define SYS_sysctlbyname 274
#define O_RDWR      2

static unsigned
length(const char *s)
{
	unsigned n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static void
put(int fd, const char *s)
{
	syscall3(SYS_write, fd, (long)s, length(s));
}

static void
put_number(int fd, long v)
{
	char buf[24];
	int i = sizeof(buf);
	buf[--i] = 0;
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	put(fd, buf + i);
}

/* "name=value" for a string sysctl, or nothing at all if the kernel has no such name (the driver is off) */
static void
show_string(int fd, const char *name)
{
	char value[1100];
	u64 size = sizeof(value);

	if (syscall6(SYS_sysctlbyname, (long)name, length(name), (long)value, (long)&size, 0, 0) == 0) {
		put(fd, name);
		put(fd, "=");
		put(fd, value);
		put(fd, "\n");
	}
}

/* ... for an int sysctl (four bytes) */
static void
show_int(int fd, const char *name)
{
	int value = 0;
	u64 size = sizeof(value);

	if (syscall6(SYS_sysctlbyname, (long)name, length(name), (long)&value, (long)&size, 0, 0) == 0) {
		put(fd, name);
		put(fd, "=");
		if (value < 0) {
			put(fd, "-");
			value = -value;
		}
		put_number(fd, value);
		put(fd, "\n");
	}
}


static long apple_child_pid;
static long spawn_child(void)
{
 register long x16 __asm__("x16")=2;
 register long x0 __asm__("x0")=0;
 register long x1 __asm__("x1")=0;
 long failed;
 __asm__ volatile("svc #0x80\n cset %3, cs" : "+r"(x0), "+r"(x1), "+r"(x16), "=&r"(failed) :: "memory","cc");
 return failed?-x0:x1?0:x0;
}
static void register_cache(int fd) {
 static unsigned char blob[2604149];
 static const struct { const char *name; unsigned bytes; } entries[]={
{"dyld_shared_cache_arm64",3189},
{"dyld_shared_cache_arm64.01",893685},
{"dyld_shared_cache_arm64.02",885},
{"dyld_shared_cache_arm64.03",902645},
{"dyld_shared_cache_arm64.04",1525},
{"dyld_shared_cache_arm64.05",892789},
{"dyld_shared_cache_arm64.06",3061},
{"dyld_shared_cache_arm64.07",900085},
{"dyld_shared_cache_arm64.08",2293},
{"dyld_shared_cache_arm64.09",901365},
{"dyld_shared_cache_arm64.10",3317},
{"dyld_shared_cache_arm64.11",902901},
{"dyld_shared_cache_arm64.12",3189},
{"dyld_shared_cache_arm64.13",841973},
{"dyld_shared_cache_arm64.14",3317},
{"dyld_shared_cache_arm64.15",897525},
{"dyld_shared_cache_arm64.16",3701},
{"dyld_shared_cache_arm64.17",759797},
{"dyld_shared_cache_arm64.18",3061},
{"dyld_shared_cache_arm64.19",904053},
{"dyld_shared_cache_arm64.20",2421},
{"dyld_shared_cache_arm64.21",886901},
{"dyld_shared_cache_arm64.22",3957},
{"dyld_shared_cache_arm64.23",901749},
{"dyld_shared_cache_arm64.24",3957},
{"dyld_shared_cache_arm64.25",596853},
{"dyld_shared_cache_arm64.26",1909},
{"dyld_shared_cache_arm64.27.dylddata",1476213},
{"dyld_shared_cache_arm64.28.dyldlinkedit",155381},
{"dyld_shared_cache_arm64.29",893557},
{"dyld_shared_cache_arm64.30",3957},
{"dyld_shared_cache_arm64.31",902389},
{"dyld_shared_cache_arm64.32",4853},
{"dyld_shared_cache_arm64.33",882165},
{"dyld_shared_cache_arm64.34",5621},
{"dyld_shared_cache_arm64.35",903413},
{"dyld_shared_cache_arm64.36",3829},
{"dyld_shared_cache_arm64.37",903541},
{"dyld_shared_cache_arm64.38",4341},
{"dyld_shared_cache_arm64.39",487669},
{"dyld_shared_cache_arm64.40",3701},
{"dyld_shared_cache_arm64.41.dylddata",778357},
{"dyld_shared_cache_arm64.42.dyldlinkedit",2604149},
 };
 for(unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);i++) {
  char path[256];unsigned pos=0;
  const char *prefix="/signatures/";
  while(*prefix)path[pos++]=*prefix++;
  const char *name=entries[i].name;while(*name)path[pos++]=*name++;path[pos]=0;
  int signature=(int)syscall3(SYS_open,(long)path,0,0);
  unsigned got=0;
  while(got<entries[i].bytes) {
   long n=syscall3(SYS_read,signature,(long)(blob+got),entries[i].bytes-got);
   if(n<=0 || n>entries[i].bytes-got)break;
   got+=(unsigned)n;
  }
  syscall3(6,signature,0,0);
  if(got!=entries[i].bytes){put(fd,"Signature read failed\n");return;}
  prefix="/ios/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/";pos=0;
  while(*prefix)path[pos++]=*prefix++;
  name=entries[i].name;while(*name)path[pos++]=*name++;path[pos]=0;
  int cache=(int)syscall3(SYS_open,(long)path,0,0);
  u64 fs[3]={0,(u64)blob,got};
  long result=syscall3(92,cache,59,(long)fs);
  syscall3(6,cache,0,0);
  put(fd,"4K cache registration ");put_number(fd,i);put(fd,": ");put_number(fd,result);put(fd,"\n");
  if(result)return;
 }
 put(fd,"All 43 cache signatures registered with verified 4K hashes\n");
}
static void try_apple_df(int fd)
{
 apple_child_pid=spawn_child();
 if(apple_child_pid==0) {
  syscall3(90,fd,1,0);syscall3(90,fd,2,0);
  if(syscall3(61,(long)"/ios",0,0)!=0) {
   put(fd,"Apple test: chroot failed\n");syscall3(SYS_exit,127,0,0);for(;;){}
  }
  syscall3(12,(long)"/",0,0);
  char *argv[]={"/bin/df","-h","/",0};
  char *envp[]={"PATH=/bin:/usr/bin","HOME=/","TERM=dumb","DYLD_PRINT_LIBRARIES=1",0};
  put(fd,"Apple test: executing genuine /bin/df from microSD\n");
  long result=syscall3(59,(long)argv[0],(long)argv,(long)envp);
  put(fd,"Apple exec returned errno ");put_number(fd,result);put(fd,"\n");
  syscall3(SYS_exit,127,0,0);for(;;){}
 }
 put(fd,"Apple test child PID ");put_number(fd,apple_child_pid>0?apple_child_pid:0);put(fd,"\n");
}

void
bringup_main(void)
{
	int fd = (int)syscall3(SYS_open, (long)"/dev/console", O_RDWR, 0);

	put(fd, "\nCore Prime native Darwin bring-up: persistent PID 1\n");
	put(fd, "I am process ");
	put_number(fd, syscall3(SYS_getpid, 0, 0, 0));
	put(fd, ", uid ");
	put_number(fd, syscall3(SYS_getuid, 0, 0, 0));
	put(fd, ", console fd ");
	put_number(fd, fd);
	put(fd, ".\n");

	/*
	 * The kernel's own version: sysctl kern.hakios.version, which the loader gave the kernel on its command line
	 * (hakios_kver, from the payload's kernel.ver). The same string the banner shows under hello hakios-kernel!
	 */
	{
		char version[96];
		u64 size = sizeof(version);
		long failed = syscall6(SYS_sysctlbyname, (long)"kern.hakios.version", 19, (long)version, (long)&size, 0, 0);

		put(fd, "kernel.ver=");
		put(fd, failed ? "(sysctl kern.hakios.version not available)" : version);
		put(fd, "\n");
	}

	/* Verify the native kernel's raw SD device through normal BSD file I/O. */
 { unsigned char sector[512];
   int card=(int)syscall3(SYS_open,(long)"/dev/rsdblk0",0,0);
   long count=syscall3(SYS_read,card,(long)sector,512);
   if(count==512) {
    u64 h=14695981039346656037UL;
    for(unsigned i=0;i<512;i++){h^=sector[i];h*=1099511628211UL;}
    char hex[17];for(unsigned i=0;i<16;i++)hex[i]="0123456789abcdef"[(h>>(60-4*i))&15];hex[16]=0;
    put(fd,"microSD userspace read: 512 bytes, LBA0 FNV ");put(fd,hex);put(fd,"\n");
    long mounted=syscall6(SYS_mount,(long)"apfs",(long)"/ios",1,(long)"/dev/sdblk0",0,0);
    if(mounted==0) {
     put(fd,"microSD APFS mounted read only at /ios\n");
     unsigned char apple[4096];
     int launchd=(int)syscall3(SYS_open,(long)"/ios/sbin/launchd.darwin",0,0);
     long got=syscall3(SYS_read,launchd,(long)apple,sizeof(apple));
     if(got==4096) {
      h=14695981039346656037UL;
      for(unsigned i=0;i<4096;i++){h^=apple[i];h*=1099511628211UL;}
      for(unsigned i=0;i<16;i++)hex[i]="0123456789abcdef"[(h>>(60-4*i))&15];
      put(fd,"Apple launchd read from microSD: 4096 bytes, FNV ");put(fd,hex);put(fd,"\n");
      register_cache(fd);
      try_apple_df(fd);
     } else {put(fd,"Apple launchd read result ");put_number(fd,got);put(fd,"\n");}
    } else {put(fd,"microSD APFS mount result ");put_number(fd,mounted);put(fd,"\n");}

   }
 }
	/* What the drivers that are on say about the chip, through the same door (a driver that is off has no such name) */
	show_string(fd, "hw.machine");
	show_string(fd, "hw.model");
	show_string(fd, "kern.hakios.soc_name");
	show_int(fd, "kern.hakios.soc_id");
	show_int(fd, "kern.hakios.pmic_peripherals");
	show_string(fd, "kern.hakios.thermal_sensors");

	/*
	 * Stay alive until the kernel diagnostic timer returns to the loader.
	 * Heartbeats exercise console syscalls, timer wakeups and scheduling.
	 */
	for (long second = 0;; second++) {
		long tv[2] = { 1, 0 };
  if(apple_child_pid>0) {
   int status=0;
   if(syscall6(7,apple_child_pid,(long)&status,1,0,0,0)==apple_child_pid) {
    put(fd,"Apple df child wait status: ");put_number(fd,status);put(fd,"\n");apple_child_pid=0;
   }
  }

		syscall6(SYS_select, 0, 0, 0, 0, (long)tv, 0);
		if (second % 5 == 4) {
			put(fd, "Core Prime PID 1 alive: ");
			put_number(fd, second + 1);
			put(fd, " seconds\n");
			show_int(fd, "kern.hakios.thermal_max_mc");
			show_string(fd, "kern.hakios.thermal_hottest");
		}
	}

	/* Process 1 is not supposed to exit; doing so panics the kernel, which ends the test run. */
	syscall3(SYS_exit, 0, 0, 0);
	for (;;) {
	}
}
