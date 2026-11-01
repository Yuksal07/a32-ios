#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the xnu port needs to boot.

TARGET: Samsung Galaxy A32 4G (SM-A325F), MediaTek Helio G80 (MT6769T).
Only what the early boot path actually reads (see NOTES.md). Values marked
[iomem] / [dtbo] come from galaxy_a32_backup (iomem_a32.txt, extracted_dtb_*.dts);
values marked [UNVERIFIED] are standard MT6768/MT6769 numbers that the backup
does not contain (the base DTB / live /proc/device-tree is still needed).
Usage:  mkadt.py OUT.adt [--dump] [--qemu] [--highmem]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM. [live FDT] /memory reg = <0x40000000 0x1_80000000> (6 GiB total).
# [iomem] The upper part is one contiguous System RAM range 0xc0000000-0x1bfff4fff
# (about 4 GiB, no holes) and is above every low-DRAM carve-out (tee_reserved_mem
# 0xbff00000, ramoops 0x4d010000, LK ramdisk 0x47c80000). Default (no --highmem)
# is a conservative 1 GiB test window at its start.
PHYS_BASE = 0xc0000000
MEM_SIZE = 0x40000000
# Panic log buffer, carved from the TOP of the dram range and exposed to XNU as
# /pram + /chosen/embedded-panic-log-size. Without it, pe_init.c's
# check_for_panic_log() leaves gPanicBase == 0, debug_log_init() bails out twice
# and the kernel hangs right after "kdp_core zlib memory" — reproduced on QEMU
# and independently on ios-fajita.
PANIC_LOG_SIZE = 0x100000
# --highmem (phone only): the whole contiguous upper bank from iomem
# (0xc0000000 .. 0x1bff00000, last 0xf5000 left unused).
# Needs the eMMC/DMA path to handle >4 GiB addresses (MSDC DMA is 34-bit capable
# on MT6768 - [UNVERIFIED]; the stage-1 PIO probe does not care).
HIGH_PHYS_BASE = 0xc0000000
HIGH_MEM_SIZE = 0xfff00000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

# GICv3 (GIC-500) [live FDT /interrupt-controller reg, VERIFIED]:
#   GICD 0x0c000000 size 0x40000, GICR 0x0c040000 size 0x200000 (1 redistributor region).
GICD, GICD_SIZE = 0x0c000000, 0x40000
GICR, GICR_SIZE = 0x0c040000, 0x200000
# Splash framebuffer [live FDT /chosen atag,videolfb raw struct, PROBABLE]:
#   fb_base 0x7c720000, vramSize 0x022e0000, panel 's6e3fc3'. The memory hole in
#   iomem (0x7c720000-0x7ea40fff is not System RAM) matches this exactly.
#   (The dtbo overlay's 0x7e605000 is a placeholder; do not use it.)
FB, FB_SIZE = 0x7c720000, 0x22e0000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic (unchanged)
# 1080x2400 [dtbo lcm_params-resolution]. Stride 4352 = 1088 px * 4 [fb0 sysfs: stride 4352,
# virtual_size 1088x4800, PROBABLE for the LK splash buffer; sweet used 4320].
FB_W, FB_H, FB_STRIDE = 1080, 2400, 1088 * 4
# Internal storage is eMMC on MSDC0 [live FDT /msdc@11230000, VERIFIED]: reg 0x11230000
# size 0x10000, SPI 100 (INTID 132), 8-bit, HS400/HS200/DDR 1.8 V, non-removable;
# msdc0_top 0x11cd0000. cmdline: androidboot.boot_devices=...soc/11230000.mmc.
# No xnu driver exists yet, so this node is informational (see HARDWARE_A32.md).
EMMC, EMMC_SIZE = 0x11230000, 0x10000
EMMC_SPI = 100
# Debug UART0 [live FDT]: serial@11002000 (UART1 @ 0x11003000); retail cmdline has
# mtk_printk_ctrl.disable_uart=1, so there is no usable serial. TOPRGU 0x10007000 [VERIFIED].
UART0 = 0x11002000
# [live FDT /timer clock-frequency 0xc65d40] = 13 MHz (VERIFIED, also BogoMIPS 26.00)
TIMEBASE = 13000000
# [live FDT /timer interrupts PPI 13,14,11,10] => virtual timer PPI 11 = INTID 27 (VERIFIED)
VTIMER_INTID = 27
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py)."""
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    # Carve the panic log off the top so the kernel never allocates over it.
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # MT6769V/CT: eight cores in physical-core order. [photo] /proc/cpuinfo:
    # cpu0-5 part 0xd05 = Cortex-A55, cpu6-7 part 0xd0a = Cortex-A75 (verified).
    # One homogeneous cluster for xnu.
    # MPIDR [live FDT /cpus, VERIFIED]: reg = core << 8 for all eight cores
    # (0x000..0x500 = A55, 0x600/0x700 = A75), i.e. the same Aff1=core layout the
    # original port used; PSCI method smc, enable-method psci.
    # The kernel defaults to one core and uses cpus=N to select a prefix.
    # QEMU retains its original single-CPU tree: its MPIDRs differ from the phone.
    def mpidr(core):
        return core << 8

    cpu_nodes = []
    for core in range(1 if qemu else 8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('arm,cortex-a55' if core < 6 else 'arm,cortex-a75') + cstr('ARM,v8')),
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # per-core private L2 (A55/A75)
            ('l3-cache-id', u32(0)),     # shared L3 (DSU)
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2000000000 if core >= 6 else 1800000000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1600000000)),  # [UNVERIFIED] LPDDR4X
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else VTIMER_INTID)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, 0x10000000, 0x100000000 - 0x10000000)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    # eMMC host (MSDC0). No xnu driver yet: the old bsd/dev/sweet_ufs.c looks
    # for 'sweet-ufs', which is intentionally NOT emitted any more, so the UFS
    # root path stays inert. Boot stage 1 uses a RAM-disk root (mkpayload.py
    # --ramdisk, boot-arg rd=md0) until a MediaTek MSDC driver exists.
    emmc = node('sweet-emmc', [
        ph(),
        ('compatible', cstr('mediatek,mt6769-mmc')),
        ('reg', u64(EMMC, EMMC_SIZE)),
        ('msdc-spi', u32(EMMC_SPI)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Panic log region. /pram reg = <phys size>; it sits just above the
    # dram-size we expose, so XNU maps it but never allocates into it.
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'samsung,a32' is what SweetPlatformExpert + boot CPU driver match (patches 0016, 0018).
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('samsung,a32') + cstr('mediatek,mt6769t') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product + ([] if qemu else [emmc]))


if __name__ == '__main__':
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
