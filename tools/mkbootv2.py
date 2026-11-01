#!/usr/bin/env python3
"""Minimal Android boot.img v2 writer (no mkbootimg needed).
usage: mkbootv2.py OUT --kernel F --ramdisk F --dtb F [--base 0x40000000 --kernel-off 0x80000
       --ramdisk-off 0x7c80000 --tags-off 0xbc80000 --dtb-off 0xbc80000 --page 2048
       --os-version 12.0.0 --patch 2025-03 --cmdline STR --name STR]"""
import argparse, hashlib, struct
p = argparse.ArgumentParser()
p.add_argument('out'); p.add_argument('--kernel', required=True); p.add_argument('--ramdisk', required=True)
p.add_argument('--dtb', required=True)
for k, d in (('base', '0x40000000'), ('kernel-off', '0x80000'), ('ramdisk-off', '0x7c80000'),
             ('tags-off', '0xbc80000'), ('dtb-off', '0xbc80000')):
    p.add_argument('--' + k, default=d)
p.add_argument('--page', type=int, default=2048); p.add_argument('--os-version', default='12.0.0')
p.add_argument('--patch', default='2025-03'); p.add_argument('--cmdline', default='bootopt=64S3,32N2,64N2 loop.max_part=7')
p.add_argument('--name', default='SRPTL10A012')
a = p.parse_args()
rd = lambda f: open(f, 'rb').read()
k, r, d = rd(a.kernel), rd(a.ramdisk), rd(a.dtb)
base = int(a.base, 0)
x, y, z = (int(v) for v in a.os_version.split('.'))
yy, mm = (int(v) for v in a.patch.split('-'))
osv = ((x << 14 | y << 7 | z) << 11) | ((yy - 2000) << 4) | mm
cmd = a.cmdline.encode()
assert len(cmd) < 512
ident = hashlib.sha1(k + r + d).digest().ljust(32, b'\0')
hdr = b'ANDROID!' + struct.pack('<10I', len(k), base + int(a.kernel_off, 0), len(r), base + int(a.ramdisk_off, 0),
      0, 0, base + int(a.tags_off, 0), a.page, 2, osv)
hdr += a.name.encode().ljust(16, b'\0') + cmd.ljust(512, b'\0') + ident + b'\0' * 1024
hdr += struct.pack('<IQII', 0, 0, 1660, len(d)) + struct.pack('<Q', base + int(a.dtb_off, 0))
assert len(hdr) == 1660, len(hdr)
pad = lambda b: b + b'\0' * (-len(b) % a.page)
open(a.out, 'wb').write(pad(hdr) + pad(k) + pad(r) + pad(d))
print('wrote', a.out, 'os_version=0x%x' % osv)
