#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/usr/lib/llvm-18/bin:$PATH
mkdir -p out
for file in start.S diag.c; do
 clang --target=aarch64-none-elf -mcpu=cortex-a53 -O2 -ffreestanding -fno-builtin \
  -fno-stack-protector -fno-pic -mstrict-align -mgeneral-regs-only -Wall -Wextra \
  -c "$file" -o "out/${file%.*}.o"
done
ld.lld -T shim.ld --build-id=none -o out/diag.elf out/start.o out/diag.o
llvm-objcopy -O binary out/diag.elf out/Image
python3 - <<'PY'
from pathlib import Path
import subprocess,struct
symbols=subprocess.check_output(['llvm-nm','out/diag.elf'],text=True)
end=next(int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1]=='_end')
p=Path('out/Image')
with p.open('r+b') as f:f.truncate(end)
h=p.read_bytes()[:64]
assert h[56:60]==b'ARM\x64'
assert struct.unpack_from('<Q',h,16)[0]==end
print('ARM64 diagnostic Image:',end,'bytes')
PY
# C objects must not embed pointers tied to the link address 0.
if llvm-readelf -r out/diag.o | grep -q 'R_AARCH64_ABS64'; then
 echo 'Unexpected absolute C relocation' >&2; exit 1
fi

# Package for lk2nd: append the phone DTB to the Image and record the DTB
# offset in the Image header at 0x2C. lk2nd's dev_tree_appended() reads that
# offset (Qualcomm convention); without it, fastboot boot fails "dtb not found".
DTB=/home/laptop/Desktop/Nura-rossa/dtb/rossa-g360t1.dtb
MKBOOTIMG=/home/laptop/Documents/Lineage/lineage-16.0/system/core/mkbootimg/mkbootimg
cp out/Image out/Image-dtb
DTB_OFFSET=$(stat -c %s out/Image)
cat "$DTB" >> out/Image-dtb
python3 - "$DTB_OFFSET" <<'PY'
import struct,sys
p='out/Image-dtb'
d=bytearray(open(p,'rb').read())
struct.pack_into('<I',d,0x2C,int(sys.argv[1]))
open(p,'wb').write(d)
print('Image-dtb: DTB offset',hex(int(sys.argv[1])),'written at 0x2C,',len(d),'bytes')
PY
if [ ! -f out/empty.cpio.gz ]; then
 python3 - <<'PY'
import gzip
# minimal newc cpio trailer, byte-identical to the working artifact
fields=[0,0,0,0,1,0,0,0,0,0,0,11,0]  # ino..check; nlink=1, namesize=11
hdr='070701'+''.join('%08x'%v for v in fields)
assert len(hdr)==110
arc=(hdr+'TRAILER!!!\x00').encode()+b'\x00'*3
assert len(arc)==124
gzip.open('out/empty.cpio.gz','wb').write(arc)
PY
fi
python3 "$MKBOOTIMG" --kernel out/Image-dtb --ramdisk out/empty.cpio.gz \
 --base 0x80000000 --kernel_offset 0x80000 --ramdisk_offset 0x02000000 \
 --tags_offset 0x01e00000 --pagesize 2048 \
 --cmdline 'console=null lk2nd.pass-simplefb=autorefresh,xrgb8888 lk2nd.pass-ramoops' \
 -o out/diagnostic-boot.img
sha256sum out/diagnostic-boot.img
