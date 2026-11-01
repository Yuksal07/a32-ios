#!/bin/bash
# Build the diagnostic shim boot.img with clang + lld only (no aarch64-gcc, no mkbootimg).
# usage: build-clang.sh [payload]   -> out/shim-boot.img
set -euo pipefail
cd "$(dirname "$0")"; mkdir -p out
CF="${DIAG:+-DDIAG} ${DX:+-D$DX} ${ES:+-DES=$ES} ${LOG:+-DLOG} --target=aarch64-linux-gnu -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
for f in start.S shim.c console.c mmu.c fdt.c loader.c mmc.c; do clang $CF -c $f -o out/${f%.*}.o; done
ld.lld -N -T shim.ld --build-id=none out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/mmc.o -o out/shim.elf
llvm-objcopy -O binary out/shim.elf out/Image
end=$(llvm-nm out/shim.elf | awk '$3=="_end"{print $1}'); truncate -s $((16#$end)) out/Image
if [ -n "${1:-}" ]; then cp "$1" out/ramdisk.bin; else (cd out && rm -rf rd && mkdir rd && cd rd && echo x > x && echo x | cpio -o -H newc --quiet 2>/dev/null | gzip -9 > ../ramdisk.bin || true); fi
python3 ../tools/mkbootv2.py out/shim-boot.img --kernel out/Image --ramdisk out/ramdisk.bin --dtb ../dtb/boot_unpacked/dtb
