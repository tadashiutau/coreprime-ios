#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH=/usr/lib/llvm-18/bin:$PATH
HAKIOS=../hakios
SHIM=../coreprime-shim
mkdir -p out
for file in start.S loader.c; do
 clang --target=aarch64-none-elf -mcpu=cortex-a53 -O2 -ffreestanding -fno-builtin \
  -fno-stack-protector -fno-pic -mstrict-align -mgeneral-regs-only -Wall -Wextra \
  -I"$SHIM" -c "$file" -o "out/${file%.*}.o"
done
ld.lld -T shim.ld --build-id=none -o out/loader.elf out/start.o out/loader.o
llvm-objcopy -O binary out/loader.elf out/loader-Image
python3 - <<'PY'
from pathlib import Path
import subprocess,struct
symbols=subprocess.check_output(['llvm-nm','out/loader.elf'],text=True)
end=next(int(line.split()[0],16) for line in symbols.splitlines() if line.split()[-1]=='_end')
p=Path('out/loader-Image')
with p.open('r+b') as f:f.truncate(end)
h=p.read_bytes()[:64]
assert h[56:60]==b'ARM\x64'
assert struct.unpack_from('<Q',h,16)[0]==end
print('ARM64 loader Image:',end,'bytes')
PY
llvm-readelf -r out/loader.o > out/loader-relocations.txt
if grep -q 'R_AARCH64_ABS64' out/loader-relocations.txt; then
 echo 'Unexpected absolute C relocation' >&2; exit 1
fi
# XNUP payload: the kernel built by the hakios toolchain plus the hello ramdisk.
# debug/serial/rd match what the port's UEFI loader puts on the command line;
# serial=3 is what sends kernel console output to the serial port at all.
python3 "$HAKIOS/xnuloader/mkpayload.py" -o out/payload.bin \
 kernel="$HAKIOS/hakios-kernel" ramdisk="$HAKIOS/userland/hello" \
 --cmdline 'debug=0x8 serial=3 rd=md0 cpus=1 hello_wait=10' $*
