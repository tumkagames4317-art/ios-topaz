# ios-topaz

**Apple's iOS 16 running on a Redmi Note 12 4G.**

This is a port of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (Apple's open-source XNU kernel, xnu-8796, on a Redmi Note 10 Pro) to the Xiaomi **Redmi Note 12 4G** (`topaz` / `tapas`, Qualcomm **SM6225 "khaje"** / Snapdragon 685). On top of it runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd and SpringBoard.

As of **2026-10-19** the tree boots to the **iOS lock screen** in about 3 minutes on the four Cortex-A73 cores. Display uses **direct MDP scanout** (no GPU compositor yet). Touch, power/volume keys, battery gauge and Gold cpufreq are in-tree (patches 0083–0094); treat phone-side confirmation as incomplete unless you have a matching boot log.

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed, and Apple's security model is mostly switched off. It installs into the `userdata` partition and so **erases Android's user data**. Nothing is flashed to the boot partitions; the kernel is started from RAM with `fastboot boot`.

## What works

| Area | State |
| --- | --- |
| Boot | Stock Xiaomi ABL → `fastboot boot` of our shim (boot image v4) → XNU. Boot slots and other OSes on the phone are not touched |
| CPU | The four Cortex-A73 ("Gold", MPIDR `0x100`–`0x103`) cores, 16K pages, GICv3, timers. Gold cluster raised to max cpufreq by default (patch 0094; `sweetcpufreq=0` to skip). The Silver cores are parked (see below) |
| Memory | Upper DDR bank for XNU (`mkadt.py --highmem` default `0x1_40000000` + 4 GiB). The shim overrides `dram-base`/`dram-size` and `boot_args` from ABL's `/memory` high bank (≥4 GiB), so the same image covers 4 GiB and 6 GiB SKUs |
| Storage | UFS host driver (UFS 3.0 at `0x4804000`); the iOS root is a journaled HFS+ volume in `userdata`, read/write. GPT watchdog fences LUN0 headers (patch 0088) |
| iOS userland | Real iPhone X 16.7.16 root filesystem and shared cache; launchd and the system daemons run |
| Display | Kernel IOSurface + IOMobileFramebuffer (`AppleCLCD`); CoreAnimation software-renders; full-screen surfaces can be pointed at MDP VIG0 for direct scanout (`sweetfb_direct`, default on). Splash FB at `0x5c000000` (1080×2400). Presented to iOS as an iPhone X panel |
| GUI | backboardd, SpringBoard, boot Apple logo, lock screen |
| Touch / keys | In code by default: FT5452 on QUP0 SE2 (0083–0090); PM6125 power / volume (0087). Disable with `sweettouch=0` / `sweetkeys=0`. Confirm with `SWEET TOUCH:` / `SWEET KEYS:` console lines |
| Battery | SM5602 gauge + bq2589x-class charger over QUP0 SE1 (`sweetbattery=1` default; `=0` static 100 %) |
| Wall clock | PM6125 RTC via SPMI (0127 read, 0128 write): `SWEET RTC:` on console; GPS-epoch adjust unless raw looks like Unix; settimeofday persists when `write_ee==0`. `sweetrton=0` / `sweetrtoffset=N` |
| Device identity | iPhone X (`iPhone10,3` / `D22AP`): no Home button, gesture UI |

## What does not work (yet)

- **Phone confirmation of input** may still be incomplete on some builds/firmware; use the console probes above.
- **Wi-Fi incomplete** (`modem=1 wifi=1`: **0130–0146** assoc + WPA2 + HTT + `tw0` + IOEthernetController; family built as `iokitcpp`; `sweetwlanif=0` leaves the IONetwork ifnet only; no IO80211/configd confirmation/resolv). **No Bluetooth, cellular, audio, camera, sensors, GPS, SEP.** `bluetoothd` / `fairplayd.H2` relaunch.
- **No GPU** (Adreno 610 / Mesa plan below); UI is CPU + MDP scanout only.
- **RTC** read/write are in-tree (0127/0128); confirm `SWEET RTC: unix …` / `wrote unix …` on device. If ownership is not EE0, write is skipped.
- **Only 4 of 8 cores** (see "Why only the Gold cores").
- **USB device/ECM** still gated behind `sweethw=1` and is SM7150-shaped (**unsafe on topaz**).
- **Security is off**: permissive AMFI and sandbox, stub AppleKeyStore, no data protection.

## How topaz differs from sweet

These are the walls hit during the port; each one stops the boot silently if missed.

| Problem | Fix |
| --- | --- |
| Xiaomi's topaz ABL ignores boot image v1/v2 and drops the boot v4 ramdisk (it only passes vendor_boot's) | The shim is packed as boot v4 and the XNU payload is appended to the shim's kernel Image after `_end` (`shim/build.sh`, `shim/loader.c`) |
| ABL starts on a Kryo 260 **Silver** core (Cortex-A53 class, MIDR `0x51AF8014`), which has **no 16K translation granule** (`ID_AA64MMFR0.TGran16 = 0`) | The shim wakes Gold CPU `0x100` with PSCI `CPU_ON`, the Silver core parks in `wfi`, and XNU only ever runs on Gold. The ADT lists the four Gold cores (`tools/mkadt.py`, patch 0082) |
| The cores are **ARMv8.0**: no LSE atomics, no RCpc (`ldapr`), no PAN. Apple clang keeps its `apple-m1` CPU default under `-arch arm64` even with `-march` | Kernel and `libfirehose_kernel` are built with `-mcpu=cortex-a53`, PAN is off, the commpage PFZ `casa` became an LDAXR/STXR loop (patch 0081, `xnu.yml`) |
| The iPhone X userland still contains LSE atomics and `ldapr` | They are emulated atomically in `handle_uncategorized` (LDAXR/STLXR on the user address, copyio recovery) (patch 0081) |
| XNU hangs after `initCPU` on Gold (no SGI / timer interrupt) | The shim prepares the GIC for all Gold cores before the jump: GICD groups, redistributor wake, SGIs/PPIs in Group 1 (`test_gic()` / `gold_gicr_prepare()` in `shim/shim.c`) |
| Different MMIO map | GIC `0xf200000`/`0xf300000`, UFS `0x4804000`, splash framebuffer `0x5c000000`, PS_HOLD `0x440b000`, IMEM restart reason `0xc12565c`, TLMM tiles WEST `0x500000` / SOUTH `0x900000` / EAST `0xd00000` |

### Why only the Gold cores

XNU for iOS uses 16K pages for the kernel and the userland on every CPU, and the Silver cores cannot translate 16K pages. Using all eight cores needs a 4K-page XNU, which is on the roadmap as an experiment.

## How it boots

```
ABL (stock Xiaomi bootloader, unlocked)
  -> fastboot boot shim-boot.img        (boot image v4, RAM only)
     -> shim on Silver CPU0: prints MMFR0/MIDR, wakes Gold CPU 0x100, parks
        -> shim on Gold: GIC setup, speed probe, finds the payload behind _end
           -> XNU Mach-O + Apple Device Tree (ADT) + boot-args
              -> XNU -> UFS driver -> HFS+ root in userdata
                 -> dyld + shared cache -> launchd -> backboardd + SpringBoard
```

There is no serial console. The kernel draws its console into a cached shadow / write-combined path that the MDP can scan (patches 0095–0099); with `sweetfb_noconsole=1` text stops overlaying the GUI after the first iOS frame (panics are still drawn). The kernel also keeps sweet's persistent log ring at the end of `userdata` and its user stack sampler (`SMP name tN pc .. bt ..`).

The shim screen prints `MEMFDT` / `MEMSZ` when it takes the high bank from ABL's DTB (else `MEMFDT using payload`). Rebuild the shim after pulling this change (`shim/build.sh`); no new XNU patch is required.

## How to do it yourself

You need a Redmi Note 12 4G (`topaz`/`tapas`, SM6225) with an **unlocked bootloader**, a Linux PC with `fastboot`, and a GitHub account for the kernel build. **Everything on `userdata` is lost.** Boot, vendor_boot, firmware and other partitions are never written.

### 1. Build the kernel

The kernel builds on GitHub's macOS runners (macos-14, Xcode 15) with [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build); newer Xcode's `iig` does not match the xnu-8796 IORPC ABI. Fork the repo and run, on the `topaz` branch:

```sh
gh workflow run xnu.yml -R YOU/ios-topaz --ref topaz
gh run download RUN_ID -R YOU/ios-topaz -n xnu-kernel -D xnu/out
```

A build takes about 8 minutes. If the artifact upload times out on GitHub's side, rerun the job. Check that the kernel has no LSE left (`llvm-objdump -d --mattr=+v8.5a ... | grep -P '\t(cas|ldadd|ldset|ldclr|swp|ldapr)'`; the only hit should be the `K256` SHA table).

### 2. Build the iOS root image

Download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple. On Linux, with `apfs-fuse` (build it from [sgan81/apfs-fuse](https://github.com/sgan81/apfs-fuse) if your distribution has no package), `hfsprogs` and sudo:

```sh
# OS, SystemOS and AppOS disk images (see BuildManifest.plist, D22AP Erase identity)
sudo bash tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
```

The device tree builder copies `/product` from the real iPhone X device tree. Take `Firmware/all_flash/DeviceTree.d22ap.im4p` from the IPSW, decompress its LZFSE payload (`ipsw img4 im4p extract`) and save it as `research/ipsw-x/d22ap.adt`.

### 3. Build the boot image

Needs `aarch64-linux-gnu-gcc` and AOSP `mkbootimg.py` (set `MKBOOTIMG` if it is not at the default path in `shim/build.sh`).

```sh
python3 tools/mkadt.py shim/out/topaz.adt --highmem
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/topaz.adt shim/out/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 sweetfb_stayon=1'
shim/build.sh ../shim/out/payload.bin      # -> shim/out/shim-boot.img
```

| Boot-arg | Effect |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Root on `userdata`, writable |
| `cpus=4` | All four Gold cores; `cpus=1` is the safe fallback |
| `sweetfb_noconsole=1` | Stop drawing kernel text over the GUI after the first iOS frame. Leave it out to debug |
| `sweetfb_hz=30` | Present / fade timer rate (Hz) |
| `sweetfb_direct=0` | Disable MDP direct scanout (fallback to CPU copy into the splash FB) |
| `sweetfb_stayon=1` | Ignore display-off requests (useful until wake from keys/touch is solid) |
| `sweettouch=0` | Disable the FT5452 driver |
| `sweettouchdelay=N` | Seconds to wait before touch bring-up |
| `sweettouchirq=0` | Disable IRQ-hinted polling (default on topaz: poll faster when GPIO80 is low) |
| `sweettouchdiag=1` | Panic after probe with bring-up values; `=2` panic after ~30 s of poll stats |
| `sweetkeys=0` | Disable PM6125 power/volume HID |
| `sweetbattery=0` | Static 100 % battery instead of SM5602/bq2589x |
| `sweetcpufreq=0` | Do not force Gold to max LUT entry |
| `sweetrton=0` | Disable PM6125 RTC bring-up |
| `sweetrtoffset=N` | Add N seconds to raw RTC (default GPS epoch `315964800`; `-1` = use raw) |
| `modem=1` / `wifi=1` | Boot MPSS + WCN3988 bring-up / scan (needs firmware under `/usr/share/firmware/topaz/`) |
| `wifissid=Name` | After scan, associate to matching BSS (0130 open; 0131 also privacy/WPA2) |
| `wifipsk=Secret` | WPA2-PSK passphrase for in-kernel 4-way (0132) |
| `wifiip=A.B.C.D` | Static IPv4 on `tw0` after authorize (0134) |
| `wifiip=dhcp` / `wifidhcp=1` | In-kernel DHCP client on `tw0` (0135; T1 renew 0136) |
| `wifimask=A.B.C.D` | Netmask for static `wifiip=` (default `255.255.255.0`) |
| `wifigw=A.B.C.D` | Optional default gateway (static 0134; also from DHCP options) |
| `wifidns=A.B.C.D` | Store DNS for sysctl/ioreg with static `wifiip=` (0143; DHCP uses opt 6) |
| `wifigarpon=0` | Skip gratuitous ARP after `tw0` address (0137) |
| `topazwlannub=0` | Skip `TopazWlan` IOService nub (0140) |
| `sweetwlanif=0` | Do not attach stub ifnet `tw0` (0145: use the IOEthernet ifnet only) |

Sysctl (0138–0142, needs `wifi=1`): `kern.topaz_wlan.ready` / `associated` / `authorized`; `sysctl -w kern.topaz_wlan.psk=Secret` (or boot-arg `wifipsk=`); `scan=1`; `sysctl kern.topaz_wlan.bss`; `sysctl -w kern.topaz_wlan.connect=SSID`; `sysctl kern.topaz_wlan.status` (ready/assoc/auth + ip/mask/gw/dns); `sysctl kern.topaz_wlan.dns` (DHCP resolver or empty). ioreg `TopazWlan` also publishes `TopazWlanIPv4` / `Netmask` / `Gateway` / `DNS` (0142).
| `sweethw=1` | Also start sweet's SM7150 USB bring-up (**unsafe on topaz**, wrong MMIO) |
| `launchd_unsecure_cache=1`, `wdt=-1 wdt_test=no_user_checkin` | launchd without AMFI; no hardware watchdog |

### 4. Flash the root and boot

```sh
fastboot -S 512M flash userdata ios16-root.simg   # ~5 minutes; do not unplug
fastboot boot shim/out/shim-boot.img
```

The shim screen (red) shows `MMFR0`, the Gold hand-off and `SPEED silver=… gold=…` for 8 seconds, then XNU starts. The first boot spends a while on the Apple logo (SpringBoard waits for LaunchServices to build its database); the lock screen follows after about 3 minutes. To get back to fastboot, hold Power + Volume down.

### Debugging tips

- Stack samples (`SMP name[pid] slide S mh M` then `SMP name tN pc .. bt ..`): symbolicate shared-cache addresses with `ipsw dyld a2s dyld_shared_cache_arm64 <addr - S>`.
- `SWEET TOUCH: ...` lines show each touch bring-up step (clocks, SE2 firmware check, chip id `54:52`, fw/vendor/mode).
- `SWEET CPUFREQ: gold perf state ...` confirms max frequency vote.
- `SWEET FB: MDP VIG0 ...` / INTF1 refresh lines confirm direct scanout.

## Roadmap

1. **Confirm input on hardware** — pull `SWEET TOUCH:` / `SWEET KEYS:` from the persistent log or on-screen console; fix any remaining FT firmware quirks.
2. **Confirm RTC on hardware** — `SWEET RTC: unix …` / `wrote unix …`; tune `sweetrtoffset` if the epoch is wrong.
3. **Confirm 6 GB on hardware** — shim already picks ABL `/memory` high bank (`MEMFDT`/`MEMSZ` on the shim screen, ADT `dram-*` patched). Verify a 6 GiB topaz reports `MEMSZ` ≈ 6 GiB and boots.
4. **Narrow the GIC fix** to the register that actually matters, then move it into XNU.
5. **All 8 cores (experiment, later):** build XNU with 4K pages (drop `__ARM_16K_PG__` from `SWEET.h`, as XNU's own `BCM2837` Raspberry Pi 3 config does on Cortex-A53) and describe two clusters, Gold as `P` and Silver as `E`. Risk: iOS 16 userland assumes 16K in malloc, JIT, IOSurface, commpage. Keep the 16K Gold-only kernel as fallback.
6. **Silver cores as a coprocessor** (idea): bare-metal shim code for panel work instead of XNU.
7. **GPU** (2026-10-19 plan): kernel Adreno 610 driver (topaz-woa `TopazGpuW`) + msm DRM-style user client for Mesa freedreno so QuartzCore's GLES path can run; Metal later.
8. **Wi-Fi** — HTT + `tw0` + IONetworkingFamily (`iokitcpp`, 0146) + `TopazWlan` `IOEthernetController` (0113–0146). Next: CI, then confirm configd sees the link; IO80211 later ([topaz-woa](https://github.com/MakrSas/topaz-woa) `TopazWifi`).
9. **Speaker** (topaz-woa `TopazAudio` + `TopazSpeaker`, `docs/P9_audio.md`): ADSP / AudioReach into an XNU audio driver for CoreAudio.
10. **Also ready in topaz-woa**: brightness, USB host/OTG (`TopazOtgDxe` + XHCI), RPM (`TopazRpm`), SIM/LTE (`docs/P9_sim.md`).

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm), boot image v4, Gold hand-off, GIC setup, payload loader; FDT `/memory` → ADT/boot_args highmem |
| `xnu/patches/` | XNU patch series (applied in order by CI): sweet's 0001–0079, topaz's 0080–0146 |
| `.github/workflows/xnu.yml` | Kernel CI build |
| `tools/mkadt.py` | Apple Device Tree for topaz (Gold cores, memory map, MMIO) and the iPhone X identity |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh`, `tools/sweet-root-tweaks.py` | Build and adjust the HFS+ iOS root |
| `tools/sweet-crash.py`, `sweet-log.py` | Symbolicate crash reports, read the persistent log |
| `NOTES.md` | Active topaz handoff at the top; sweet engineering log below (history) |

Not in git: partition backups, live device trees, Apple firmware and root images, build trees.

## topaz kernel patches

| # | What |
| --- | --- |
| 0080 | SM6225 CPU topology; initially gate sweet SM7150 key/touch/USB behind `sweethw=1` |
| 0081 | ARMv8.0: `-mcpu=cortex-a53`, no PAN, PFZ without CASA, userland LSE/LDAPR emulation |
| 0082 | Boot CPU is Gold MPIDR `0x100` |
| 0083 | Touch: FocalTech FT5452 on QUP0 SE2 (from [topaz-woa](https://github.com/MakrSas/topaz-woa) TopazTouch); touch init no longer needs `sweethw` |
| 0084 | `sweettouchdiag` panic probe for bring-up steps |
| 0085 | `sweetfb_stayon=1` ignores display-off (auto-lock) |
| 0086 | FT parser stats (`sweettouchdiag=2`) |
| 0087 | Touch RCG fixes; PM6125 power/volume keys; keys+touch default on (`sweetkeys=0` / `sweettouch=0` to disable); USB still `sweethw=1` |
| 0088 | UFS GPT watchdog: refuse writes that would clobber LUN0 GPT; panic if headers change |
| 0089 | Extra FT probe dumps (fw/vendor/mode, bad-frame raw) |
| 0090 | TD_STATUS `0xff` idle frames = release all (HyperOS FT firmware) |
| 0091 | Battery: QUP0 SE1 I2C + SM5602 / bq2589x into SweetBattery (`sweetbattery`) |
| 0092 | Direct MDP VIG0/CTL0 scanout for eligible full-screen IOSurfaces (`sweetfb_direct`) |
| 0093 | Scanout surfaces from contiguous RAM (not a fixed PA pool) |
| 0094 | Gold cluster max cpufreq (`sweetcpufreq`, default on) |
| 0095 | Write-combined boot framebuffer console mapping |
| 0096 | Smooth display-off fade in cached memory |
| 0097 | Console on cached shadow frame scanned by MDP |
| 0098 | INTF1 refresh diagnostics |
| 0099 | On-screen surfaces stay in-use; batched console flush |
| 0100–0126 | Display path, backlight, modem/WLAN bring-up (see commit history) |
| 0127 | PM6125 RTC → wall clock (`sweet_rtc.c`, SweetPlatformExpert getGMTTimeOfDay) |
| 0128 | PM6125 RTC settimeofday via SPMI EXT_WRITEL (chnls @0x01e00000) |
| 0129 | WLAN stub ifnet `tw0` + `<sys/topaz_wlan.h>` on radio ready hook |
| 0130 | Open-network association (`wifissid=`, topaz-woa assoc.c) |
| 0131 | WPA2-PSK assoc + key install API + HTT TX/RX EAPOL hook |
| 0132 | In-kernel WPA2-PSK 4-way (`topaz_wlan_wpa.c`, `wifipsk=`) |
| 0133 | HTT MSDU ↔ `tw0` ethernet datapath (`topaz_wlan_tx_ethernet`) |
| 0134 | `tw0` static IPv4 (`wifiip=` / `wifimask=` / `wifigw=`) |
| 0135 | `tw0` in-kernel DHCP (`wifiip=dhcp` / `wifidhcp=1`) |
| 0136 | DHCP lease T1 renew (option 51; DNS logged only) |
| 0137 | Gratuitous ARP after `tw0` IPv4 (`wifigarpon=0` off) |
| 0138 | `kern.topaz_wlan` sysctl scan / BSS list / connect |
| 0139 | Runtime WPA2-PSK via `kern.topaz_wlan.psk` (`topaz_wlan_set_psk`) |
| 0140 | `TopazWlan` IOService nub (ioreg properties; not IONetwork) |
| 0141 | Restore assoc teardown; clear `tw0` IP on disconnect |
| 0142 | Expose `tw0` IP/DNS via `kern.topaz_wlan.status`/`dns` + TopazWlan ioreg |
| 0143 | `wifidns=` stores DNS for static `wifiip=` (sysctl/ioreg; not resolv) |
| 0144 | Stage/wire OSS IONetworkingFamily-170.140.2 (`sweet_ionet` + IONetworkStack) |
| 0145 | `TopazWlan` `IOEthernetController` + interface (HTT bridge; `sweetwlanif=0` drops `tw0`) |
| 0146 | Build IONetworkingFamily as `iokitcpp` (`*.cpo` include path + warning waivers) |

## Key hardware facts (topaz)

- SoC SM6225 "khaje": 4× Kryo 260 Silver (A53 class, MPIDR `0x0`–`0x3`) + 4× Kryo 260 Gold (A73 class, `0x100`–`0x103`). Only Gold has the 16K granule.
- DDR banks `0x40000000`+`0x3bb00000`, `0x80000000`+3 GiB, `0x1_40000000`+4 GiB; all fixed carve-outs are below `0x63900000`.
- GICv3 at `0xf200000` (redistributors `0xf300000`, stride `0x20000`), virtual timer INTID 19, 19.2 MHz.
- Panel: 1080×2400 DSI video mode with DSC, continuous splash at `0x5c000000`.
- Touch: FocalTech FT5452 at `0x38` on QUP0 SE2, raw range 1350×3000 (1.25× the panel); IRQ GPIO80, reset GPIO86, AVDD GPIO36.
- Keys: PM6125 PON (power / vol−); volume up on PM6125 GPIO5.
- Battery bus: QUP0 SE1 — SM5602 @ `0x71`, charger @ `0x6a`.
- More hardware notes for this phone: [MakrSas/topaz-woa](https://github.com/MakrSas/topaz-woa) (`docs/AGENT_BRIEF_drivers.md`).

## Credits

Based on [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), which did the hard part: XNU on a Qualcomm phone with the iPhone X userland. Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Qualcomm/Xiaomi's published Linux kernel sources, and the topaz hardware work in [MakrSas/topaz-woa](https://github.com/MakrSas/topaz-woa). iOS itself is Apple's and is not distributed here.
