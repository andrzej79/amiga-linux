# Warp drivers (firmware 3.000+)

| Driver | File | Binds to | Kconfig |
|---|---|---|---|
| core | `drivers/mfd/cswarp-core.c` | Zorro CTRL board (0x1400/101) | `MFD_CSWARP` |
| network | `drivers/net/ethernet/amiwarpnet.c` | MFD child `cswarp-net` | `AMIWARPNET` |
| IDE | `drivers/ata/pata_cswarp.c` | MFD child `cswarp-ata` | `PATA_CSWARP` |
| RTG framebuffer | `drivers/video/fbdev/amiwarpfb.c` + `amiwarp_vidclk.c` | MFD child `cswarp-fb` (+ Zorro VRAM board) | `FB_AMIGA_WARP` |

The core refuses to bind, and none of the others load, when either of these is true:
- The FPGA has no DPREG register file: the ID at CTRL+0x1004 is not `0x80570001`.
- The board firmware is older than release 3.000 (`0x3000`). Dev builds (`0x0900`) are accepted.

The framebuffer also requires SysCfg ID ≥ `0x01234569` and `PIXCLK_SR` bit 2 (the DRP pixel clock interface).

## Kernel command line

| Option | Meaning |
|---|---|
| `cswarp_core.dbg=<mask>` | debug messages, printed at KERN_INFO (default `0xa9`) |
| `cswarp_core.use_irq=0` | no INT2 handler: poll the handshake and the doorbells |
| `cswarp_core.xfer_timeout_ms=<n>` | frame exchange timeout (default 5000) |
| `cswarp_core.spin_us=<n>` | busy-wait before sleeping on the ARM (default 100) |
| `pata_cswarp.pio32=1` | 32-bit PIO (Warp560/1240/1260 only) |
| `video=amiwarpfb:1024x768@60,depth:16` | initial mode and depth |
| `amiwarpfb.noaccel=1` | software fill and copy instead of the blitter |
| `amiwarpfb.vram_cached=0` | map VRAM uncached instead of write-through |

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

The mask can be changed at runtime: `echo 0x1ff > /sys/module/cswarp_core/parameters/dbg`.

## sysfs (the CTRL Zorro device, `/sys/bus/zorro/devices/<id>/`)

- `firmware_version`, `board`, `native_ide`
- `stats`: exchanges, timeouts, bad replies, IRQ counts, IRQs missed (handshake found by polling), and per-unit doorbells
- `regs`: CR, ID, IRQ_IF/IE/PEND, PIXCLK_SR, IRQ_SR/CR
- `recover` (write): re-enable the frame after a timeout. After a timeout every board request fails, because a late reply from the ARM would otherwise land in the next caller's frame.

The framebuffer adds `accel_stats` (fill, copy and pan counts, blitter wait timeouts, current pixel clock) on the `cswarp-fb` platform device.

## First hardware test: what to look for
1. `cswarp-core: FPGA: DPREG id 0x80570001, SysCfg id ...`, then the board, firmware and IDE-mode line.
2. `stats` after some network traffic:
   - `irq_missed` should stay 0. If it doesn't, the INT2 path isn't working and the driver is running on the polling fallback.
   - `timeouts` should stay 0.
3. If the machine hangs early, boot with `cswarp_core.use_irq=0`.
4. If the screen is black, check the `fb:` debug lines (pixel clock status, register dump). Then try `amiwarpfb.noaccel=1`, or a 640x480 mode.
5. If the 32 bpp fill colours are wrong, the blitter's colour word format is the suspect. It is currently the native pixel value from the pseudo-palette.
