# Warp drivers (firmware 3.000+)

| Driver | File | Binds to | Kconfig |
|---|---|---|---|
| core | `drivers/mfd/cswarp-core.c` | Zorro CTRL board (0x1400/101) | `MFD_CSWARP` |
| network | `drivers/net/ethernet/amiwarpnet.c` | MFD child `cswarp-net` | `AMIWARPNET` |
| IDE | `drivers/ata/pata_cswarp.c` | MFD child `cswarp-ata` | `PATA_CSWARP` |
| RTG framebuffer | `drivers/video/fbdev/amiwarpfb.c` + `amiwarp_vidclk.c` | MFD child `cswarp-fb` (+ Zorro VRAM board) | `FB_AMIGA_WARP` |
| SD card / USB disks | `drivers/block/cswarp_disk.c` | MFD child `cswarp-disk` | `BLK_DEV_CSWARP` |
| board control | `drivers/mfd/cswarp-ctl.c` | MFD child `cswarp-ctl` | `MFD_CSWARP` |

The core refuses to bind, and none of the others load, when either of these is true:
- The FPGA has no DPREG register file: the ID at CTRL+0x1004 is not `0x80570001`.
- The board firmware is older than release 3.000 (`0x3000`). Dev builds (`0x0900`) are accepted.

The framebuffer also requires SysCfg ID ≥ `0x01234569` and `PIXCLK_SR` bit 2 (the DRP pixel clock interface).

## Kernel command line

| Option | Meaning |
|---|---|
| `cswarp_core.dbg=<mask>` | debug messages, printed at KERN_INFO (default `0x2a9`) |
| `cswarp_core.use_irq=0` | no INT2 handler: poll the handshake and the doorbells |
| `cswarp_core.xfer_timeout_ms=<n>` | frame exchange timeout (default 5000) |
| `cswarp_core.spin_us=<n>` | busy-wait before sleeping on the ARM (default 100) |
| `pata_cswarp.pio32=1` | 32-bit PIO (Warp560/1240/1260 only) |
| `video=amiwarpfb:1024x768@60,depth:16` | initial mode and depth |
| `amiwarpfb.noaccel=1` | software fill and copy instead of the blitter |
| `amiwarpfb.vram_cached=0` | map VRAM uncached instead of write-through |
| `cswarp_disk.poll_ms=<n>` | media change polling interval (default 1000; 0 = scan only at probe) |
| `cswarp_disk.io_timeout_ms=<n>` | fail a disk request when its unit announces nothing for this long (default 30000) |
| `cswarp_disk.max_kb=<n>` | largest disk request, also sets the readahead window (default 1024; `max_sectors_kb` in sysfs can lower it at runtime) |
| `cswarp_disk.dma=0` | move all disk data through the frame (PIO) instead of sending requests larger than 3.5 KB over the QSDMA engine (default 1; also writable at runtime in `/sys/module/cswarp_disk/parameters/dma`) |
| `cswarp_disk.zc_min_kb=<n>` | with `dma=1`, a read chunk goes straight into the request's pages when they form physically contiguous runs averaging at least this many KB, each on whole L2 lines; otherwise through the bounce buffer (default 32; 0 = always bounce; runtime-writable) |

Debug mask bits:

| Bit | Class |
|---|---|
| 0x001 | core: probe, identity, quiesce, units |
| 0x002 | mbox: every frame exchange, with its round-trip time |
| 0x004 | irq: every INT2 entry that was ours |
| 0x008 | net: interface state, link, MAC, queue stop/wake |
| 0x010 | net-pkt: every TX/RX frame |
| 0x020 | fb: mode set, pixel clock, register dump |
| 0x040 | fb-accel: every fill, copy and pan |
| 0x080 | ata: probe, IDE timing, registers |
| 0x100 | ata-io: every PIO data transfer |
| 0x200 | disk: unit caps, media changes |
| 0x400 | disk-io: every disk request and media poll |

The mask can be changed at runtime: `echo 0x1ff > /sys/module/cswarp_core/parameters/dbg`.

## sysfs (the CTRL Zorro device, `/sys/bus/zorro/devices/<id>/`)

- `firmware_version`, `board`, `native_ide`
- `stats`: exchanges, timeouts, bad replies, IRQ counts, IRQs missed (handshake found by polling), and per-unit doorbells
- `regs`: CR, ID, IRQ_IF/IE/PEND, PIXCLK_SR, IRQ_SR/CR
- `recover` (write): re-enable the frame after a timeout. After a timeout every board request fails, because a late reply from the ARM would otherwise land in the next caller's frame.

The disk driver adds `disk_stats` (per unit: reads, writes, bytes, errors, timeouts, cancels, busy retries, media changes, DMA state and chunk counts, zero-copy chunks/runs and why chunks were bounced, and where request time went in us: `total`, `submit`, `wait` for the board, `xfer` fetch exchanges, `tc`, `cache` maintenance, `copy` bounce copies) on the `cswarp-disk` platform device. Writing anything to it zeroes the counters.

The framebuffer adds `accel_stats` (fill, copy and pan counts, blitter wait timeouts, current pixel clock) on the `cswarp-fb` platform device.

## Board control (`cswarp-ctl`, used by `warptool`)

The Linux side of AmigaOS WarpTool, on the platform device `/sys/bus/platform/devices/cswarp-ctl/`:

| File | Meaning |
|---|---|
| `board/diag` | everything `dpcmdGetDiag` reports, one `key value` per line: board, type, versions, Kickstart slot, 68k clock and phase, temperatures (m°C), 5 V and battery (mV), USB host status, IDE mode and timings, fan settings and output, WiFi SSID/state/RSSI/channel, video output, L2 geometry, L2 early read ack. The WiFi password is not in it. |
| `board/wifi_password` | the stored WiFi password (root only) |
| `board/cpu_clock_hz` | the 68k clock; writing Hz (25-60 MHz on 68040 boards, 33-120 MHz on 68060 boards) reprograms the board's PLL at once. The board keeps the setting. `loops_per_jiffy` is rescaled, so `udelay()` and `/proc/cpuinfo` follow. |
| `l2/size_kb`, `l2/ways`, `l2/line_bytes` | L2 geometry |
| `l2/policy` | `lru` or `legacy`; writing also flushes the L2 and zeroes the counters. Hidden on FPGA images without the register (SysCfg id < `0x01234568`). |
| `l2/early_ack` | `on`/`off` plus `default`/`user`; write `on`, `off` or `default` (fw 30.22+; `EOPNOTSUPP` on an FPGA core without the switch) |
| `l2/stats` | `accesses`, `hits`, `misses`, `writebacks`; any write zeroes them (SysCfg id >= `0x01234569`) |
| `audio/master`, `paula`, `ahi`, `mhi`, `line_out`, `speaker` | `<dB>` or `<dB> mute`; write a dB value (sets and unmutes), `mute` (keeps the level) or `unmute`. The board refuses levels outside the channel's range (`ERANGE`). fw 30.54+. |
| `audio/state` | codec state and counters, and `channel <name> <dB> <min> <max> <muted>` lines |

The diag reply is cached for 0.5 s. A hwmon device `warp` (`/sys/class/hwmon/hwmonN`; `sensors` from lm-sensors, which is in the image, shows it as `warp-isa-0000`) has `temp1`-`temp4` (68k internal, 68k NTC, ARM, FPGA), `in0`/`in1` (+5V, Vbatt) and `pwm1` (fan output, 0-255, read-only).

`warptool` (userspace, `warptool/` in the repo) takes WarpTool's keywords: `Diag` (default), `WiFiShowPass`, `CPUFreqHz=105.5M`, `L2Info`, `L2Policy=lru|legacy`, `L2EarlyAck=on|off|default`, `L2Stats`, `L2StatsReset`, `Audio`, `volMaster=`/`volAmiga=`/`volAhi=`/`volMhi=`/`volLineOut=`/`volSpeaker=` (dB or `mute`).

## SD card and USB disks (`cswarp-disk`)

The board's ARM owns the SD slot and the USB ports. It offers each medium as an async unit (`AUNIT_SD`, `AUNIT_USB0/1`), the interface `warpSD.device` and `warpUSBDisk.device` use. Disks are `warpsd`, `warpusb0` and `warpusb1`. They appear when a medium is found and disappear when it is removed (polled once a second). Partitions follow the usual naming (`warpsd1`, `warpusb0p1`); MBR and Amiga RDB tables are both read. Requests of up to 3.5 KB go through the frame (PIO); larger ones go over the QSDMA engine, a whole chunk per exchange (`dma=0` sends everything through the frame). A read chunk goes straight into the request's pages when they form a few physically contiguous runs (page cache reads usually do); otherwise it goes into one half of a per-unit bounce buffer (two staging halves) and is copied out with `move16` while the next chunk is pushed into the other half. Writes always go through the bounce buffer. Every DMA target must be in Warp RAM and own whole L2 lines (128 B), otherwise the unit stays on PIO or the chunk is bounced. The CPU caches and the board's L2 are flushed around every transfer.

Measured on an A3000/Warp7060 (68060 at 104 MHz) with a high-speed USB SD reader, 1 MB requests: buffered `dd` from the disk 5.2 MB/s PIO, 12 MB/s DMA; `iflag=direct` 23 MB/s DMA. Buffered reads are CPU-bound: the page cache to user copy alone runs at about 28 MB/s on this CPU. USB optical drives and the USB floppy are not handled.

## First hardware test: what to look for
1. `cswarp-core: FPGA: DPREG id 0x80570001, SysCfg id ...`, then the board, firmware and IDE-mode line.
2. `stats` after some network traffic:
   - `irq_missed` should stay 0. If it doesn't, the INT2 path isn't working and the driver is running on the polling fallback.
   - `timeouts` should stay 0.
3. If the machine hangs early, boot with `cswarp_core.use_irq=0`.
4. If the screen is black, check the `fb:` debug lines (pixel clock status, register dump). Then try `amiwarpfb.noaccel=1`, or a 640x480 mode.
5. If the 32 bpp fill colours are wrong, the blitter's colour word format is the suspect. It is currently the native pixel value from the pseudo-palette.
