# Claude continuation handoff — native Darwin/iOS on Samsung Galaxy Core Prime

Prepared November 3, 2026, about 17:41 America/New_York. This document is the newest handoff. It supersedes older status and next-step claims in ZCODE-HANDOFF.md. Those older sections are historical evidence, not the current phone state.

## Your task and the user's authorization

Continue the attempt to boot genuine iOS userland natively on the user's Samsung Galaxy Core Prime SM-G360T1 (MSM8916). The selected userland is iOS 16.5 / 20F66, iPhone10,3. QEMU is a development aid; the deliverable is the real phone. A working XNU kernel or our own static PID1 is not full iOS. Be precise about what has actually executed.

The user wants autonomous progress with frequent, concise reports, and the system filesystem on the A1 microSD because internal storage is small. They authorize losing all phone/card data, but want the phone recoverable. Preserve Samsung Download mode, bootloader chain, working recovery, EFS/radio/calibration, and verified backups. Do not request another formatting/data-loss confirmation for the already-authorized experiment. Do not promise every failure is recoverable.

Codex is PAUSED at the user's explicit request so you can take over. There are no Codex phone tests, writes, or builds still running. Resume development only when the user gives you this continuation task. Do not concurrently coordinate another assistant onto the same phone without user authorization.

## Immediate state at pause

- Phone is healthy in **lk2nd fastboot**, confirmed by `fastboot -s 45693dc9 devices`.
- BOOT still contains lk2nd. All Codex tests this session used **fastboot boot**, RAM-only. No internal partition was flashed or formatted this session.
- The microSD p1 contains the installed, fully readback-verified system APFS image.
- Native XNU mounts the SD APFS volume, reads real Apple files, and runs genuine Apple dyld. The latest test successfully registered all 43 cache signatures and dyld listed real Apple system libraries.
- The Apple `/bin/df` child then exited with **SIGILL**, raw wait status **4**. Its parent stayed alive and the kernel returned normally after the diagnostic timer. **df has not completed successfully. Apple launchd and SpringBoard have not run.**
- An opt-in diagnostic kernel build to capture the SIGILL opcode/PC/LR finished successfully just before pause. **It has NOT been tested on the phone or packaged into a new boot image.**
- Current `work/hakios/hakios-kernel` SHA256:
  `6ffe7141e3700874cf981f4b267fa9f34e653913adbb00d5e869faa48d74d116`
- Last hardware-tested kernel SHA256 was:
  `dfb25330c0be18f1ab3e37801fbebc4a0935d54d6a178e637fa09766e6dd4791`
  The saved successful-milestone images still contain that tested kernel.

## Workspace and files to read first

Actual workspace:
`/home/laptop/Documents/Codex/2026-11-03/ho`

User-facing project home:
`/home/laptop/Documents/SamsungIOS/`

SamsungIOS has README.md and work/outputs symlinks to the actual workspace. Nothing was moved. Run commands from the actual workspace unless specified otherwise. There is no Git repo at the workspace root; `work/hakios` is a nested Git checkout, HEAD `96ffbc532e970f3540885755d457e401e04d4707`.

Read:
1. This handoff.
2. `outputs/core-prime-native-bootstrap/REPORT-native-console-persistent.md`, especially its latest appended sections; earlier limitations were superseded.
3. `outputs/core-prime-native-bootstrap/hardware-logs/ramoops-xnu-cache-4k-120.txt` — latest authoritative native result.
4. `work/coreprime-xnu-loader/bringup-cache-4k.c`, `prepare-cache-signatures.py`, `package-persistent.sh`, `run-phone.py`, and `extract-run.py`.
5. `outputs/ZCODE-HANDOFF.md` only for older background, hardware quirks, and its later LIVE STATE UPDATE 4. Its opening Download-mode/pre-first-boot claims are obsolete.

Use work/ for code/intermediates and outputs/ for reports. Do not delete/reset source changes. Do not repeat broad cleanup: earlier cleanup already reclaimed approximately104.6GiB from generated Lineage intermediates/caches. About154GiB remained free at the latest check.

Protected from cleanup/reset:
- This entire active Codex workspace and its backups/toolchains/logs.
- `/home/laptop/Documents/Lineage/` (`/home/laptop/android` points there).
- `/home/laptop/Desktop/Galaxy Core Prime - Driver RE Data/`.
- `/home/laptop/Desktop/Nura-rossa/`.
- `/home/laptop/.local/var/pmbootstrap/`.
- All stock firmware, partition dumps, radio/EFS, recovery and rollback images.

This protection does not prohibit normal source edits for the authorized project. It prohibits treating active project material or backups as disposable build clutter.

## What is verified on hardware

1. Native AArch64 XNU8796.121.2 / Darwin22.5 on the real Cortex-A53.
2. Working VM, GICv2, virtual timer PPI20, scheduling, console and persistent static PID1.
3. The user personally saw Darwin text and `Core Prime PID 1 alive: 5 seconds`, then10,15,etc. on the panel.
4. IPSW-derived compatibility identity `hw.machine=iPhone10,3`, `hw.model=D22AP`. The actual hardware descriptions/MMIO remain MSM8916; this identity does not turn the CPU into an Apple CPU.
5. A read-only retained-state microSD SDHCI backend exposes `/dev/sdblk0` and `/dev/rsdblk0`.
6. Native APFS mounts the SD image at `/ios`, and the first4096 bytes of `/ios/sbin/launchd.darwin` match host FNV64 `ca01410707207832`.
7. Real Apple `/bin/df` exec reaches real Apple dyld. After the detached-signature workaround below, dyld loads/lists libutil, libSystem, libc++, libdispatch, libsystem_kernel, libobjc, and other system libraries from the shared cache, then the child dies SIGILL.
8. Parent PID1 continues heartbeats and the timed diagnostic return takes the phone back to lk2nd without kernel panic.

Not working/proven: completed Apple command, Apple launchd, GUI/SpringBoard, writable native SD filesystem, functional swap, audio/network/touch, autonomous boot into iOS without host RAM boot, production reliability. Current kernel maps a contiguous approximately512MiB RAM window; this is not a proven permanent hardware memory ceiling. Latest test had hundreds of MiB available, so do not assume the SIGILL is memory exhaustion.

## Exact current blocker and the workaround that already succeeded

First Apple df attempt failed in dyld:
`dyld cache '(null)' not loaded: code signature registration for shared cache failed`.
Then libutil could not be found because it lives inside the cache. Child raw wait status6 = SIGABRT.
Evidence: `hardware-logs/ramoops-xnu-sd-apple-df-120.txt`.

The direct cache probe used F_ADDFILESIGS_RETURN97 on the main cache and .01. Both returned errno85/EBADEXEC. Kernel said:
`disallowing unsupported code signature page shift: 14`.
Evidence: `hardware-logs/ramoops-xnu-cache-probe-90.txt`.
Native VM pages are4096, while Apple's cache CodeDirectories use16384-byte hash pages.

Do NOT merely permit pageSize14 in `cs_validate_codedirectory`: `cs_validate_hash` assumes a complete aligned hash page and can overread or panic for4K faults. We did not make that unsafe kernel change or disable validation.

The original cache files already have ad-hoc CodeDirectories (flags2) and empty CMS. `prepare-cache-signatures.py`:
- reads all43 caches from the extracted IPSW directory;
- verifies EVERY original SHA256 code slot against original file bytes;
- rejects non-ad-hoc or nonempty-CMS signatures and unsupported formats;
- derives SHA256 slots at4096-byte granularity, preserving the covered bytes and relevant metadata;
- writes detached signatures to `work/coreprime-xnu-loader/out/cache-signatures/`;
- leaves original cache bytes and the installed SD untouched.

Manifest: `outputs/core-prime-native-bootstrap/cache-signatures-4k.json`.
Total detached data21,235,239 bytes,43 files; largest2,604,149 bytes.

`bringup-cache-4k.c` loads these tables from the RAM boot APFS `/signatures` directory and registers each with fcntl F_ADDSIGS59 against the actual SD cache vnode. All43 registrations returned0 on the phone. Dyld's later registration finds these already-attached blobs, and real libraries load. These are locally derived ad-hoc signatures, not newly Apple-authenticated signatures. File content hashing is retained.

The parent then forks, duplicates the console fd to stdout/stderr, chroots the child to `/ios`, chdirs `/`, and execs genuine `/bin/df -h /` with DYLD_PRINT_LIBRARIES=1. Parent polls wait4/WNOHANG and remains alive. Raw wait status4 is the new blocker.

## Next action: identify SIGILL, without changing semantics

A diagnostic-only kernel is already built. It adds opt-in `user_fault_log=1` printf in:
- `handle_uncategorized`;
- `handle_wf_trap`;
- `handle_fp_trap`;
- `handle_msr_trap`;
inside generated XNU `osfmk/arm64/sleh.c`.
It prints handler name, PC, LR, and instruction opcode after COPYIN. No instruction emulation or bypass has been added.

Build log: `work/kernel-user-instruction-build.log` shows `CC sleh.o`, `LD mach.development.bcm2837.unstripped`, and final `make-kernel: built: hakios-kernel`. The familiar kext symbol-set checks print make errors after linking, but the wrapper copies a built kernel and reports success. Do not describe the whole toolchain as clean; distinguish these known export checks from compiler/linker errors. No `error:` compilation failure was found.

Suggested exact next native diagnostic, after revalidating the phone and sources:

```sh
cd /home/laptop/Documents/Codex/2026-11-03/ho
sha256sum work/hakios/hakios-kernel
fastboot -s 45693dc9 devices
RAMDISK_IMAGE=out/boot-apfs-cache-4k.img \
EXTRA_BOOTARGS='sdmmc=1 sdmmc_start=2048 sdmmc_sectors=249733120 user_fault_log=1' \
bash work/coreprime-xnu-loader/package-persistent.sh 120
cp work/coreprime-xnu-loader/out/xnu-phone-persistent.img \
   work/coreprime-xnu-loader/out/xnu-phone-cache-4k-instruction.img
python3 work/coreprime-xnu-loader/run-phone.py \
   xnu-cache-4k-instruction-120 \
   work/coreprime-xnu-loader/out/xnu-phone-cache-4k-instruction.img
rg -n 'user-instruction-fault|user-fault|registration|dyld\[|child wait|panic\(cpu' \
   outputs/core-prime-native-bootstrap/hardware-logs/ramoops-xnu-cache-4k-instruction-120.txt
```

These commands have NOT been executed with the new diagnostic kernel. The existing `out/boot-apfs-cache-4k.img` already holds the tested static program and tables; no SD reinstall or userland rebuild is needed.

Use the actual opcode to choose the next fix. Plausible hypotheses include CPU capability/comm-page dispatch choosing an instruction unavailable on Cortex-A53, a trapped system-register instruction, or a deliberate runtime trap; none is established yet. Do not blindly nop the instruction or claim all Apple code requires a newer CPU. Check capability advertisement against actual hardware. If needed correlate PC/LR with cache mapping/slide and the extracted cache files. Unstripped kernel symbols are in `work/hakios/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/`.

## Saved working images and mutable outputs

All these large test images are RAM-boot-only and exceed BOOT capacity. Never flash them.

- `outputs/core-prime-native-bootstrap/xnu-sd-apfs-mounted.img`:
  SHA256 `1001cf7962afd107dcaf46ff8118a8893459565359a1fa83e1b3f5edeee1fe9c`.
  Known native mount/read/heartbeat fallback, timer60s, no Apple child.
- `outputs/core-prime-native-bootstrap/xnu-apple-libraries-120.img`:
  SHA256 `a7ecfb602bb3ca08ec571047056407d953372180831cd09f05f26355e2eea5d3`.
  Latest hardware-tested milestone: all43 registrations, real Apple libraries, child SIGILL, parent healthy, timer120s.
- `work/coreprime-xnu-loader/out/xnu-phone-cache-4k.img` is the same tested milestone.
- `out/xnu-phone-persistent.img` is mutable; currently also that tested cache image, but packaging overwrites it. Always use an explicit saved image path when reproducing an old result.
- `out/boot-apfs-cache-4k.img` = RAM APFS containing the static program plus signature tables.
- `out/boot-apfs-plain.img` = small RAM APFS with the proper stack-entry mount/read-only program.
- `xnu-sd-apfs-plain-before-stackfix.img` in outputs is a FAILED pre-stack-fix experiment, not a fallback.
- `out/xnu-phone-sd-apfs-mount.img`, older plain/compressed APFS experiments, and similar pre-stack-fix files are not known-good. Consult logs rather than names.
- `session4-before/` contains preserved previous loader/kernel/driver/diagnostic originals. Do not delete it.

## Build and source organization

Main sources:
- `work/coreprime-xnu-loader/`: native loader, static user programs, packaging/testing/extraction tools.
- `work/coreprime-shim/diag.c`: low-level FDT/framebuffer/ramoops support included by loader.c.
- `work/hakios/drivers/storage/hakios.virtio_blk.c`: canonical experimental SD driver plus existing BSD device glue.
- `work/hakios/xnu/files/`: canonical XNU overlays.
- `work/hakios/xnu-src/xnu-8796.121.2/`: generated/nested XNU tree actually compiled.

IMPORTANT: changing a canonical overlay does not reliably sync it into the generated tree. Explicitly copy changed canonical files to the correct generated location before building. Conversely, the latest fault diagnostics exist only in generated `sleh.c` and `apfs_vnops.c`; preserve or deliberately remove them instead of assuming upstream overlays contain them.

Saved patches:
- `outputs/core-prime-native-bootstrap/session4-hakios-source.patch`: root HakiOS Git diff, INCLUDING pre-existing Zcode work. Do not revert all of it as ours.
- `outputs/core-prime-native-bootstrap/session4-temporary-fault-diagnostics.patch`: generated-tree diagnostics compared to session4-before originals, refreshed at pause including the new instruction-fault prints.
- Prior `outputs/core-prime-xnu-bootstrap/xnu-checkpoints.patch`: Zcode's checkpoint/return fixes; preserve them.

Kernel build:
```sh
HAKIOS_CORTEX_A53=1 bash work/hakios-toolchain/build-kernel.sh > work/kernel-next-build.log 2>&1
```
Docker image `coreprime-hakios-build:local`, JOBS8, workspace bind-mounted, current user. .config uses the qemu/BCM2837 configuration plus native A53/GIC adaptations. Kernel build helper accounts for expected post-link symbol-set errors, but always inspect actual compile/link output and hash.

Loader build: `bash work/coreprime-xnu-loader/build.sh`.
This also overwrites `out/payload.bin` for an older hello baseline; package-persistent.sh separately builds the phone payload. The loader's relocation guard rejects absolute pointer relocations. Identity entries use inline char[32], NOT pointers, to survive relocation.

Static user build:
```sh
bash work/coreprime-xnu-loader/build-user.sh bringup-cache-4k.c out/bringup-cache-4k
```
ALWAYS link `user-start.S`: aligns SP16, then calls `_bringup_main`. A direct C Mach-O entry SIGBUSed on real hardware even though it passed QEMU. ARM64 XNU fork returns a child flag in x1: use the existing spawn_child implementation, not a Linux-style x0-only assumption. Existing syscall wrappers return x0 errno on failure; the fork helper explicitly checks carry.

To rebuild the signature RAM image if needed:
```sh
python3 work/coreprime-xnu-loader/prepare-cache-signatures.py > outputs/core-prime-native-bootstrap/cache-signatures-4k.json
bash work/coreprime-xnu-loader/build-user.sh bringup-cache-4k.c out/bringup-cache-4k
cp work/coreprime-xnu-loader/out/bringup-cache-4k work/coreprime-xnu-loader/out/boot-root-cache-4k/sbin/launchd
cp -a work/coreprime-xnu-loader/out/cache-signatures/. work/coreprime-xnu-loader/out/boot-root-cache-4k/signatures/
python3 work/hakios/tools/apfs/mkapfs.py \
 -o work/coreprime-xnu-loader/out/boot-apfs-cache-4k.img \
 Boot=work/coreprime-xnu-loader/out/boot-root-cache-4k
```
Directory tree must also include /dev and /ios. Already present.

QEMU helper `test-persistent.py` uses A53/GICv2/1GiB and tests parent heartbeat, identity, framebuffer changes, and20s timed return. Its payload is `out/payload.bin`; generate the intended payload explicitly before using it. It does not emulate MSM8916 SD; sdmmc guard rejects non-MSM8916 DT before MMIO. Passing QEMU is useful but not proof of native execution compatibility.

## Phone test and log rules

ALWAYS exact serial45693dc9. An unrelated ADB target192.168.50.25:5555 exists; never touch it. Never run adb operations without -s.

`run-phone.py NAME [IMAGE]`:
- confirms serial in fastboot;
- uses `fastboot boot`, never flash;
- waits for automatic return, up to150s;
- runs `fastboot -s ... oem ramoops raw`, then `get_staged`;
- extracts loader/kernel text and before/after framebuffer PNGs;
- asserts actual kernel return, program banner, timer expiry and no kernel panic.

**PASS means the parent survived and returned. It does not assert a successful mount or successful Apple child. Read the text and child status.**

Logs are under `outputs/core-prime-native-bootstrap/hardware-logs/ramoops-NAME.txt` (NOT `-kernel-log.txt` for these new runs), with raw.bin, json, PNGs and transfer log siblings. Loader clears its part of ramoops at entry. extract-run.py excludes stale kernel/snapshots unless `KERNEL RETURNED TO LOADER` and no loader exception are confirmed. Earlier stale-log/failed-loader artifacts must not be presented as successes.

Never use `fastboot oem log`: it hangs this lk2nd. Do not start a second transaction while a boot/fetch/write is running. If a test exceeds its bound, inspect the screen and USB state before sending more commands. User has seen the backlight go off and then lk2nd return normally; darkness alone is not proof of a hang or brick.

## SD system and backups

microSD:
- Model SK128, typeSD, actual249737216 512-byte sectors.
- mmcblk1 in temporary recovery; sysfs removable reports0, a quirk. Do not require1.
- Existing MBR p1 start2048, length249733120. Partition table was left intact; type7 now contains our custom APFS image.
- Kernel view uses `sdmmc=1 sdmmc_start=2048 sdmmc_sectors=249733120`.
- Installed image `work/hakios/userland/system.apfs`,2476720128 bytes.
- SHA256 `71fa6b628f8c15a3edc63c1a40700a3a36c83cea06dbe5d3b7d84c90079b1c95`.
- Full remote readback matched. Metadata `microsd-system-install.json`; image validation `ios-filesystem-verification.json`.
- Small supplemental image `work/hakios/userland/root.apfs`,4194304 bytes, contains HakiOS init/jobs/injection libraries; not yet mapped as md1 by the native loader.
- The combined system image already includes cryptex OS/App contents in place. Do not run a stock separate-cryptex mounting path blindly against it.

The local IPSW is `work/ipsw/iPhone10,3_iPhone10,6_16.5_20F66_Restore.ipsw` (6165698198bytes). Extracted rootfs/cryptex/cryptex-app and all43 dyld cache files already exist in work/hakios/sysimage. Do not redownload or reextract unnecessarily. Cache source path `work/hakios/sysimage/20F66__iPhone10,3_6/`. `PREBUILT=1 SHARE=1 WIFID=0 CHECK=1 userland/build.sh` already built and verified both APFS images. Keep source caches pristine.

Full eMMC backup:
`outputs/core-prime-backup-2026-11-03/internal-emmc.img`
7818182656 bytes, SHA256
`8fedc60883f092da857d7d027e5656bc58c9d75a3a04484abcfca91aa6f6bcfe`.
Source hash and local reread matched after removing positively identified98bytes of dd stats. Metadata/partition inventories in the same directory. Does not include RPMB or microSD.

Original Android BOOT rollback:
`outputs/core-prime-native-bootstrap/android-boot-rollback.img`
13631488bytes, verified source/local SHA in sibling file.
BOOTp16 sectors253952+26624; RECOVERYp17 sectors280576+30720. Do not confuse the original Android rollback with current lk2nd content.

## Recovery and flashing quirks — read before touching them

Samsung Download04e8:685d and lk2nd fastboot18d1:d00d are different modes. Stock Samsung has no fastboot. FRPoff, Qualcomm secureboot enabled; no need to alter secureboot/fuses.

BOOT is only13MiB. Our19–37MiB test images **must never be flashed**. `fastboot boot` stages them in RAM. lk2nd's `fastboot flash boot` writes after a512KiB prefix to preserve itself, so it is **not** the way to restore a full original BOOT image. Full BOOT rollback uses Heimdall --BOOT in Download mode, exact partition and verified image. Preserve RECOVERY and all bootloader/radio/calibration partitions.

Working temporary recovery RAM image:
`work/recovery-ramboot/recovery-appended-dtb.img`.
It uses the original backed-up recovery zImage/ramdisk and its original Samsung DTB appended to zImage. Original Samsung QCDT-form recovery image failed fastboot with "dtb not found". Recovery partition was not flashed.

Recovery ADB state is `recovery`, not `device`. Old recovery rejects shell -T; normal shell is PTY and unsafe for binary transfer. `exec-in` was byte-tested and used to stream SD image with dd+fsync; full remote checksum followed. Do not casually rerun install-sd-system.py. Recovery auto-mounted internal cache/data, so claim no internal flash/format, not that every internal byte remained unchanged. Backlight brightness0 can make recovery appear off; max255,128 worked.

Recovery boot latch may force repeated recovery. Download-mode `heimdall print-pit` with its normal reboot has cleared it historically; the latest temporary recovery returned to lk2nd with normal adb reboot, so no latch fix currently needed.

## Hardware details and later work

GICv2 distributor0x0b000000, CPU0x0b002000; timer PPI20 at19.2MHz.
Framebuffer0x8e000000,480x800,stride1920,xrgb8888.
SDHCI microSD host0x07864900/core0x07864000.
eMMC host0x07824900/core0x07824000.
Native kernel window0xa0000000+0x1ff80000. Loader staged lowerRAM kernel/payload at0x82000020; loader0x80080800. Native DTB `/home/laptop/Desktop/Nura-rossa/dtb/rossa-g360t1.dtb`.

SD backend uses retained lk2nd initialization, single-sector CMD17PIO, no DMA or IRQ dependency; bounds checks and serial mutex, timeout0xe; fails closed after read error. Loader eMMC diagnostic required waking/switching/reset/clock restoration; do not assume eMMC is initialized like SD. Do not repurpose that controller blindly.

After resolving SIGILL, progress toward Apple launchd. Existing HakiOS supports PID1-only `kern.hakios_switch_root` with string `"INCOMING OUTGOING"`, promoting a mounted volume to global root while keeping devfs. Native loader currently supplies one RAMDisk; upstream loader documents RAMDisk1 for /dev/md1. A possible next stage is SD root promotion plus small root.apfs mapped as md1 and mounted at /redwood, then /redwood/sbin/init prepares writable RAM /private/var and HakiOS injections before launchd.darwin. This is a PLAN, not implemented or tested. Read init.c/stage0 and vfs_switch_root semantics before coding. Mere child chroot was sufficient to reach the current dyld/library milestone, so don't blame root switching for the resolved page-size error.

Keep reporting actual evidence and the next uncertainty. Preserve working fallback images before replacing mutable outputs. Do not call the project completed until genuine iOS userland and its intended interface are running on hardware.
