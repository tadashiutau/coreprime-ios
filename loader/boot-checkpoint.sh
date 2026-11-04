#!/usr/bin/env bash
# Native checkpoint bisect: build a --checkpoint N XNU boot image, fastboot-boot
# it (RAM only, nothing flashed), wait for the stub's PSCI reset back to
# fastboot, then pull lk2nd's ramoops and decode it.
#   usage: bash boot-checkpoint.sh N [wait_seconds]
# Exit codes: 0 = checkpoint returned (stage 200+N in the record); 2 = phone did
# not return to fastboot (the kernel hung at or before checkpoint N).
set -euo pipefail
cd "$(dirname "$0")"
N=$1
WAIT=${2:-120}
HAKIOS=../hakios
DTB=/home/laptop/Desktop/Nura-rossa/dtb/rossa-g360t1.dtb
MKBI=/home/laptop/Documents/Lineage/lineage-16.0/system/core/mkbootimg/mkbootimg
LOGS=../../outputs/core-prime-native-bootstrap/hardware-logs

python3 "$HAKIOS/xnuloader/mkpayload.py" -o "out/payload-phone-ck$N.bin" \
  kernel="$HAKIOS/hakios-kernel" ramdisk="$HAKIOS/userland/hello" \
  --cmdline 'debug=0x8 serial=3 rd=md0 cpus=1 hello_wait=10 loader_return_after=25' \
  --checkpoint "$N" >/dev/null
python3 - "$DTB" <<'PY'
import struct,sys
d=bytearray(open('out/loader-Image','rb').read()); off=len(d)
d+=open(sys.argv[1],'rb').read()
struct.pack_into('<I',d,0x2C,off)          # lk2nd reads the appended-DTB offset here
open('out/loader-Image-dtb','wb').write(d)
PY
python3 "$MKBI" --kernel out/loader-Image-dtb --ramdisk "out/payload-phone-ck$N.bin" \
  --base 0x80000000 --kernel_offset 0x80000 --ramdisk_offset 0x02000000 \
  --tags_offset 0x01e00000 --pagesize 2048 \
  --cmdline 'console=null lk2nd.pass-simplefb=autorefresh,xrgb8888 lk2nd.pass-ramoops' \
  -o "out/xnu-phone-ck$N.img"
sha256sum "out/xnu-phone-ck$N.img"

[ "$(fastboot devices | grep -c 45693dc9)" = 1 ] || { echo "phone not in fastboot"; exit 3; }
fastboot boot "out/xnu-phone-ck$N.img" 2>&1 | tee "$LOGS/fastboot-boot-xnu-ck$N.log"

back=0
for i in $(seq 1 "$WAIT"); do
  if fastboot devices 2>/dev/null | grep -q 45693dc9; then back=$i; break; fi
  sleep 1
done
if [ "$back" = 0 ]; then echo "NO RETURN within ${WAIT}s (hang at or before checkpoint $N)"; exit 2; fi
echo "phone back in fastboot after ~${back}s"

sleep 2
fastboot oem ramoops raw >/dev/null 2>&1
fastboot get_staged "$LOGS/ramoops-xnu-ck$N.bin" >/dev/null
sha256sum "$LOGS/ramoops-xnu-ck$N.bin"
python3 ../coreprime-shim/decode.py "$LOGS/ramoops-xnu-ck$N.bin" 2>/dev/null \
  || python3 - "$LOGS/ramoops-xnu-ck$N.bin" <<'PY'
# lk2nd warm-boot writes can break the record checksum; the payload survives.
import sys
d=open(sys.argv[1],'rb').read()
off=d.find(b'CPDIAG64')
print('record offset',hex(off) if off>=0 else None)
if off>=0:
    import struct
    print('stage(raw):',struct.unpack_from('<Q',d,off+16)[0])
    print(d[off+4096:off+4096+8192].split(b'\0')[0].decode(errors='replace'))
PY
