#!/bin/bash
# Build the shim and wrap it in a boot.img v2 with the stock Samsung A32 (SM-A325F) header values.
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c mmc.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/mmc.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/mmc.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
# Header values of the real SM-A325F boot.img (read with tools/unpack_bootv2.py; page 2048, v2,
# board SRPTL10A012). The stock DTB sits at tags_addr (0x4bc80000), hence DTB_OFF == TAGS_OFF.
# Override from the environment for a different firmware.
BASE=${BASE:-0x40000000}; KERNEL_OFF=${KERNEL_OFF:-0x80000}; RAMDISK_OFF=${RAMDISK_OFF:-0x7c80000}
TAGS_OFF=${TAGS_OFF:-0xbc80000}; DTB_OFF=${DTB_OFF:-0xbc80000}; PAGESIZE=${PAGESIZE:-2048}
# HDRVER: stock header version is 2 (verified)
mkbootimg --header_version "${HDRVER:-2}" --kernel out/Image --ramdisk out/ramdisk.bin \
	--dtb ../dtb/boot_unpacked/dtb --base "$BASE" --kernel_offset "$KERNEL_OFF" --ramdisk_offset "$RAMDISK_OFF" \
	--tags_offset "$TAGS_OFF" --dtb_offset "$DTB_OFF" --pagesize "$PAGESIZE" \
	--os_version 12.0.0 --os_patch_level 2025-03 \
	--cmdline "bootopt=64S3,32N2,64N2 loop.max_part=7" -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
