#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
SOURCE=${1:-bringup.c}
DEST=${2:-out/bringup}
CLANG=/usr/lib/llvm-18/bin/clang
LINKER="$(pwd)/../hakios-toolchain/theos/toolchain/linux/iphone/bin/ld"
"$CLANG" -target arm64-apple-ios16.5 -Os -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra -c "$SOURCE" -o "$DEST.o"
"$CLANG" -target arm64-apple-ios16.5 -c user-start.S -o out/user-start.o
"$CLANG" -target arm64-apple-ios16.5 --ld-path="$LINKER" -static -nostdlib -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o "$DEST" "$DEST.o" out/user-start.o
