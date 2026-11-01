# Samsung Galaxy A32 4G (SM-A325F, MT6769T) port status

Source of facts: `galaxy_a32_backup` (iomem_a32.txt, extracted_dtb_0/1/2.dts).
The three DTBs are **DTBO overlays** (hw revisions 00/02/04 of "A32 EUR OPEN"),
not the base device tree. The base DTB / `/proc/device-tree` is still missing.

## Verified from the backup
| Item | Value | Source |
| --- | --- | --- |
| SoC | MediaTek MT6769T (Helio G80) | dtbo `compatible` |
| RAM | System RAM 0x40000000-0x1bfff4fff, contiguous 0xc0000000-0x1bfff4fff (~4 GiB) | iomem |
| UART0 / UART1 | 0x11002000 / 0x11003000 | iomem `serial` |
| I2C0..8 | 0x11007000, 08000, 09000, 0f000, 11000, 16000, 0d000, 04000, 05000 | iomem |
| USB (MTU3 + PHY) | 0x11200000, 0x11cc0000 (NOT Qualcomm DWC3) | iomem |
| PMIC wrapper | pwrap 0x1000d000 | iomem |
| Panel | Samsung s6e3fc3 AMOLED, 1080x2400, 4-lane DSI, **command mode** (dsi-mode 0) | dtbo |
| Touch | **STMicro FTS (`stm_ts`)**, IRQ = EINT 10 level; the DTBO also lists a Melfas MIP4 @0x48 variant, but only `stm_ts` is registered on this unit | dtbo + photo of /proc/interrupts |
| Charger / fuel | Silicon Mitus SM5714 (charger, flash LED, USB-PD), Samsung sec-battery | dtbo |
| Splash FB | fb_base 0x7e605000, vramSize 0x17e8000 | dtbo `videolfb` |
| Storage | no UFS; eMMC (msdc alias present) | dtbo / iomem has no UFS |

## Verified from your TWRP photos (23 pictures, 2026-10-23)
| Item | Value |
| --- | --- |
| SoC string | `Hardware: MT6769V/CT` |
| Cores | cpu0-5 = Cortex-A55 (part 0xd05, r1p0), cpu6-7 = Cortex-A75 (part 0xd0a, r3p1) |
| Arch timer | 13 MHz (BogoMIPS 26.00); virtual timer = GICv3 INTID 27 (`arch_timer`) |
| Interrupt controller | GICv3 (base addresses still not shown) |
| Storage | eMMC: `boot -> mmcblk0p36`, `userdata -> mmcblk0p49`, also mmcblk0boot0/1, rpmb |
| Display IRQs | `dsi0`, `ovl0`, `rdma0` (MT DISP pipeline, SPI via MT_SYSIRQ 238/230/231...) |
| PMIC | `mt6358-irq`, `mt6358-rtc`, `pwrkey`, `homekey`, fuel gauge via SM5714 |
| Touch | `stm_ts` on EINT 10 |
| Other | mtk-kpd, mt_wdt, mt-pmic-pwrap, mtk-msdc x2, musb-hdrc (MTU3 USB), 13040000.mali GPU |
| `/sys/firmware/fdt` | exists (the base DTB can be dumped, see below) |

### Partition map (mmcblk0pN)
p1 efs, p2 sec_efs, p3 steady, p4 **nvram**, p5 spmfw, p6 scp1, p7 sspm_1, p8 uh, p9 tee1, p10 tzar,
p11 gz1, p12 lk, p13 dtbo, p14 **vbmeta**, p15 btd, p16 **protect1**, p17 **protect2**, p18 **seccfg**,
p19 bota, p20 boot_para, p21 para, p22 misc, p23 expdb, p24 frp, p25 **nvcfg**, p26 **nvdata**,
p27 metadata, p28 keyrefuge, p29 keydata, p30 md_udc, p31 **persistent**, p32 **proinfo**,
p33 md1img, p34 param, p35 up_param, p36 **boot**, p37 recovery, p38 vbmeta_system,
p39 vbmeta_vendor, p40 efuse, p41 reserved, p42 sec1, p43 super, p44 prism, p45 optics,
p46 cache, p47 omr, p48 spu, p49 **userdata**.
(Read from wrapped screen text; double-check with `ls -l /dev/block/by-name` before any write.)

## Standard MT6768/MT6769 values still NOT confirmed ([UNVERIFIED])
GICD 0x0c000000 / GICR 0x0c100000, MSDC0 0x11230000, TOPRGU watchdog 0x10007000
(SWRST +0x14 = 0x1209), MPIDR layout A55 0x000-0x005 / A75 0x100,0x101, PSCI via SMC.
The shim prints GICD/GICR state and MPIDR on screen: compare.

## What changed in this tree
- `tools/mkadt.py`: RAM, GIC, timer, CPU/MPIDR, FB, eMMC node, no `sweet-ufs` node.
- `xnu/patches/0016`, `0018`: platform expert and boot-CPU driver now match `samsung,a32`
  (the earlier edit had removed `xiaomi,sweet` from the ADT but the patches still matched it).
- `xnu/patches/0078`: MPIDR validation accepts the MT6769T layout.
- `xnu/patches/0060`, `0072`, `0077`: Qualcomm-only drivers (SPMI keys, GENI touch, DWC3 USB)
  default to OFF so they never poke Qualcomm addresses on MediaTek.
- `shim/`: GIC/timer constants, reset path (PSCI then TOPRGU, no write to address 0),
  UFS probe replaced by a stub, framebuffer address, cntfrq-based timing,
  `build.sh` requires the stock boot.img header values instead of guessing.
- `make_img.py` removed (kernel+dtb concatenation is not a boot image).

## NOT done (needs real drivers, cannot be copied from Qualcomm code)
1. eMMC (MSDC) storage driver replacing patches 0028/0030/0034/0069.
2. Display: the panel is command mode; writing the LK splash buffer will not refresh it.
   Needs the MT DISP (OVL/RDMA/DSI) path or a one-shot flush; text on screen is NOT guaranteed.
3. Touch (STMicro `stm_ts`, bus/address not yet known), buttons/PMIC (MT6358 via pwrap), battery/charger (SM5714).
4. USB (MTU3) for log/network. Audio, Wi-Fi, modem, camera: none.

## Stage 1 target (no storage needed)
RAM-disk root, single core, all Qualcomm drivers off:
```
python3 tools/mkadt.py shim/out/a32.adt --highmem
python3 tools/mkpayload.py KERNEL shim/out/a32.adt shim/out/payload.bin --ramdisk ROOT.img \
  --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=md0 wdt=-1 sweettouch=0 sweetkeys=0 sweetusb=0 sweetbattery=0'
```
Then `shim/build.sh` with the stock header values (`tools/unpack_bootv2.py STOCK_boot.img DIR`).
Samsung has no `fastboot boot`: flash to a spare boot slot only after backing up `boot`,
`vbmeta`, `nvram`, `nvdata`, `proinfo`, `persist` from TWRP, and disable AVB (vbmeta).

---
## Status after the live-FDT pass (SM-A325F, MT6769V/CT)

VERIFIED from the real boot.img + dumped FDT:
- GICD 0x0c000000 (0x40000), GICR 0x0c040000 (0x200000); arch timer 13 MHz, PPIs 27 (virt) / 30 (phys)
- 8 cores, MPIDR = core<<8 (6x A55 + 2x A75); PSCI via smc; **entry at EL2** (`bootopt=64N2`) -> shim drops to EL1
- eMMC: MSDC0 @ 0x11230000, SPI 100 (no UFS on this device)
- Framebuffer 0x7c720000, 0x22e0000 bytes; boot.img v2: page 2048, base 0x40000000, kernel +0x80000, ramdisk +0x7c80000, tags/dtb +0xbc80000, board SRPTL10A012

UNVERIFIED (written from memory, to be confirmed by the first on-screen photo):
- LK splash stride (4352 assumed; mkadt/console use 1088*4 px layout)
- Watchdog reset (0x10007000+0x14 = 0x1209); PSCI reset is tried first
- MSDC register details in shim/mmc.c (read-only probe, prints GPT + userdata fs type)
- Display refresh: the s6e3fc3 panel is command-mode, writing the LK buffer may not refresh it

NOT WRITTEN YET:
- Kernel eMMC driver (stage-1 root = RAM-disk: `mkpayload.py --ramdisk`, boot-arg `rd=md0`, 1 core)
- Touch (stm_ts, i2c6 0x49), PMIC mt6358, charger SM5714, audio, USB, display refresh path
- Qualcomm drivers (touch/keys/usb patches 0060/0072/0077) are defaulted OFF

Flashing: Samsung has no `fastboot boot`. From TWRP: back up `boot` first
(`dd if=/dev/block/by-name/boot of=/sdcard/boot_orig.img`), then flash shim/out/shim-boot.img to `boot`.
Userdata (f2fs, encrypted, ~107 GiB) may be wiped/repartitioned later for an HFS+ root.
Never commit nvram/proinfo/persist/serials from the device backups.
