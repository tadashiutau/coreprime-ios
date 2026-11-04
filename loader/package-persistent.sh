#!/usr/bin/env bash
# Build a RAM-boot image only. Does not operate the phone.
set -euo pipefail
cd "$(dirname "$0")"
SECONDS_ON=${1:-90}
[[ "$SECONDS_ON" =~ ^[0-9]+$ ]] && ((SECONDS_ON >= 15 && SECONDS_ON <= 600))
python3 ../hakios/xnuloader/mkpayload.py -o out/payload-phone-persistent.bin \
 kernel=../hakios/hakios-kernel ramdisk="${RAMDISK_IMAGE:-out/bringup}" \
 ${RAMDISK1_IMAGE:+ramdisk1="$RAMDISK1_IMAGE"} \
 --cmdline "debug=0x8 serial=0 rd=md0 cpus=${CPUS:-1} loader_return_after=$SECONDS_ON ${EXTRA_BOOTARGS:-}"
python3 - <<'PY'
from pathlib import Path
import struct
p=Path('out/loader-Image');d=bytearray(p.read_bytes());off=len(d)
d+=Path('/home/laptop/Desktop/Nura-rossa/dtb/rossa-g360t1.dtb').read_bytes()
struct.pack_into('<I',d,0x2c,off);Path('out/loader-Image-dtb-persistent').write_bytes(d)
PY
python3 /home/laptop/Documents/Lineage/lineage-16.0/system/core/mkbootimg/mkbootimg \
 --kernel out/loader-Image-dtb-persistent --ramdisk out/payload-phone-persistent.bin \
 --base 0x80000000 --kernel_offset 0x80000 --ramdisk_offset 0x02000000 \
 --tags_offset 0x01e00000 --pagesize 2048 \
 --cmdline 'console=null lk2nd.pass-simplefb=autorefresh,xrgb8888 lk2nd.pass-ramoops' \
 -o out/xnu-phone-persistent.img
sha256sum out/xnu-phone-persistent.img
