#!/usr/bin/env python3
"""Removed: concatenating kernel + DTB does NOT produce a bootable boot.img.

The Samsung bootloader needs an Android boot image header, and the XNU kernel
does not read a Linux DTB (it needs the ADT from tools/mkadt.py). Use:

  python3 tools/mkadt.py shim/out/a32.adt --highmem
  python3 tools/mkpayload.py KERNEL shim/out/a32.adt shim/out/payload.bin --ramdisk ROOT.img --cmdline "..."
  (cd shim && BASE=.. KERNEL_OFF=.. RAMDISK_OFF=.. TAGS_OFF=.. DTB_OFF=.. PAGESIZE=.. ./build.sh ../shim/out/payload.bin)

See HARDWARE_A32.md.
"""
raise SystemExit(__doc__)
