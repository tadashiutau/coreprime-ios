# Native iOS 16.5 on the Samsung Galaxy Core Prime (SM-G360T1)

Apple's open-source XNU kernel (xnu-8796.121.2, via [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel))
running natively on a Snapdragon 410 / MSM8916 phone, booting the user's own iOS 16.5 (iPhone X, 20F66) userland
from the microSD card, up to SpringBoard's home screen, with touch and buttons.

RAM-boot only (`fastboot boot` from lk2nd); nothing is flashed. No Apple or Samsung files are in this repository:
the IPSW, the iOS system image, the dyld cache signatures derived from it, the iPhone identity extracted from it
(`loader/identity-iphone-x.h`, made by `loader/extract-identity.py`), stock firmware, backups and boot images
all stay on the build machine.

## What works on the phone (2026-11-03)

| | |
|---|---|
| XNU on all 4 Cortex-A53 cores | yes — no PSCI in the stock TrustZone: lk2nd's spin-table for cpu1, our own holding pen in the loader, cores 2/3 powered up through their APCS ACC |
| microSD system volume (APFS), read only | yes — CMD18 multi-block reads, ~11 MB/s (6.6x the original single-sector PIO) |
| Apple dyld + shared cache | yes — 4K-page re-derived ad-hoc CodeDirectories registered with F_ADDSIGS (the A53 cannot do 16K pages) |
| ARMv8 Crypto Extension | emulated in the kernel (this A53 has none; corecrypto uses AES/SHA/PMULL unconditionally) |
| Apple launchd as PID 1, logd, notifyd, backboardd, SpringBoard and ~20 daemons | yes |
| Home screen | yes — CPU-drawn, ~10 frames/s; needed `ramfs_mb=256` (/private/var was full at 128 MB and SpringBoard exited 1 in a loop) |
| Touch (Imagis IST3038B, I2C) | yes, multi-touch, apps open |
| Home, volume up (GPIO), power, volume down (PM8916 PON over SPMI) | yes |
| Display shape | `fb_aspect=1` letterbox (in test), `fbscale=1` (in test) |
| Camera, GPU, audio, Wi-Fi, cellular, persistence | not yet (camera step 1 — a read-only probe — is in the kernel) |

## Layout

- `loader/` — the native lk2nd → XNU loader (`loader.c`, `start.S`), the first user-space programs
  (`bringup-launchd.c` switches the root to the microSD and hands PID 1 to Apple's launchd), build, packaging,
  phone-run and log-extraction scripts, the cache-signature preparer, the crypto emulator and its host test.
- `shim/` — low-level FDT/framebuffer/ramoops/PSCI helpers included by the loader.
- `hakios-patches/` — everything changed in HakiOS:
  - `hakios-tracked-changes.patch`: `git diff` of the HakiOS checkout against `HAKIOS_BASE_COMMIT` (includes the
    earlier Codex/Zcode bring-up work as well as this session's);
  - `new-files/`: `drivers/input/hakios.coreprime_input.c` (touch, buttons, camera probe),
    `xnu/files/osfmk/arm64/coreprime_crypto_emul.h`, `qemu/run-darwin-prebuilt.sh`;
  - `generated-*.patch`: edits made only in the generated XNU tree (`xnu-src/…`): crypto emulation hook in
    `sleh.c`, the CPU-ID print in `commpage.c`, the once-only CPU-monitor message in `task_policy.c`;
  - `earlier-*.patch`: earlier sessions' saved patches.
- `docs/REPORT.md` — the full engineering log with every hardware result, hash and decision.
- `docs/HANDOFF-start-of-session.md` — the state at the start of this session (superseded by REPORT.md's later sections).

## Boot-args used for the home-screen runs

```
debug=0x8 serial=0 rd=md0 cpus=4 loader_return_after=600
sdmmc=1 sdmmc_start=2048 sdmmc_sectors=249733120
fbscale=2 fb_hz=20 ramfs_mb=256 coreprime_input=1
```
(`package-persistent.sh` adds the first line; `CPUS=4`, `RAMDISK_IMAGE=` boot APFS, `RAMDISK1_IMAGE=` HakiOS root image.)
