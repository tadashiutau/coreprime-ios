#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
bash build-user.sh bringup.c out/bringup
bash build.sh
python3 ../hakios/xnuloader/mkpayload.py -o out/payload.bin \
 kernel=../hakios/hakios-kernel ramdisk=out/bringup \
 --cmdline 'debug=0x8 serial=0 rd=md0 cpus=1 loader_return_after=20'
