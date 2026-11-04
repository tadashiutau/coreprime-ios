# Handoff — end of 2026-11-03 (evening)

Supersedes `HANDOFF-start-of-session.md`. Detailed evidence for every item: `REPORT.md` (and the live copy
`/home/laptop/Documents/Codex/2026-11-03/ho/outputs/core-prime-native-bootstrap/REPORT-native-console-persistent.md`).

## Where things are

- Workspace: `/home/laptop/Documents/Codex/2026-11-03/ho` (work/ = code, outputs/ = reports, logs, saved images).
- This repo: `/home/laptop/Documents/SamsungIOS/coreprime-ios` → github.com/tadashiutau/coreprime-ios (private).
  Code only; Apple/Samsung material stays in the workspace. Re-sync after changes: copy `work/coreprime-xnu-loader`
  sources (not `identity-iphone-x.h`, `out/`, `session4-before/`), regenerate `hakios-patches/` (see README).
- Phone: serial 45693dc9, lk2nd fastboot. RAM-boot only (`work/coreprime-xnu-loader/run-phone.py NAME IMAGE`,
  `RETURN_WAIT=` seconds). Never flash test images. microSD p1 = read-only iOS 16.5 system APFS (unchanged).
- Kernel build: `HAKIOS_CORTEX_A53=1 bash work/hakios-toolchain/build-kernel.sh` (copy canonical overlay files into
  `work/hakios/xnu-src/xnu-8796.121.2/…` first). Latest kernel SHA256 e3c494d8… (letterbox + console hide + blank).

## Working on hardware

4 cores; microSD CMD18 reads; crypto emulation; Apple launchd/SpringBoard home screen (~10 fps, CPU drawn);
IST3038B multi-touch (apps open); Home, Volume Up, Power (locks/unlocks iOS), Volume Down.
Saved milestone images in `outputs/core-prime-native-bootstrap/`: `xnu-ios-home-screen-600.img`,
`xnu-ios-touch-home-600.img`, `xnu-4cpu-launchd-120.img`, `xnu-apple-df-complete-120.img`, …

Home-screen recipe: `CPUS=4 RAMDISK_IMAGE=out/boot-apfs-launchd.img RAMDISK1_IMAGE=out/root-launchd-ui-quiet.apfs
EXTRA_BOOTARGS='sdmmc=1 sdmmc_start=2048 sdmmc_sectors=249733120 fbscale=2 fb_hz=20 ramfs_mb=256 coreprime_input=1'
bash package-persistent.sh 600` (in `work/coreprime-xnu-loader`).

## Pending / unverified at stop

- Run `xnu-combo-600` (image `out/xnu-phone-combo.img`) was started with `fbscale=1 fb_aspect=1 coreprime_cam=1`:
  letterbox, 1x drawing, console hidden after the first frame, black screen on display-off, camera step-1 probe.
  Its log `outputs/core-prime-native-bootstrap/hardware-logs/ramoops-xnu-combo-600.txt` was not yet read. If 1x
  looks broken, go back to `fbscale=2` (keep `fb_aspect=1`).
- Camera plan (front SR200PC20 YUV sensor first; rear SR544 is raw and much harder): step 1 read-only probe
  (built, result pending) → step 2 power (RT5033 LDO1 2.8 V / BUCK1 1.8 V over bit-bang I2C GPIO 22/23, PM8916 LDO16
  cam_vio via RPM — check whether already on), MCLK1 GPIO 27, reset GPIO 28, standby GPIO 17, CCI0 GPIO 29/30, read
  chip ID → step 3 CSID test pattern into memory → step 4 sensor frame with Samsung's
  `sr200pc20_yuv_coreprime.h` tables → step 5 iOS Camera app (HakiOS camera_device.c may help).
- Backlight off: needs a DCS 0x51 command over DSI; today "off" just blacks the framebuffer.
- GPU (Adreno 306 / a3xx via Mesa freedreno, HakiOS's EAGL path): large project, not started.
- Samsung kernel source for this board: `/home/laptop/Documents/Lineage/lineage-16.0/kernel/samsung/msm8916`
  (rossa_tmo defconfig, imagis_core3 touch, i2c-msm-v2, spmi-pmic-arb, clock-gcc-8916, camera_v2 + sr200pc20).
