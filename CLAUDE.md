# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Linux for the Amiga with the CS-Lab **Warp** turbo board (68040/68060). It is not a buildable tree. It holds the pieces you add to an upstream Linux kernel (6.9.0) and to an upstream Buildroot checkout: configs, a patch, out-of-tree driver sources, a root filesystem overlay, and the `amiboot-5.6` AmigaOS bootloader binary. That binary is kept only as an archive copy. There is no test suite or linter. `build.sh` builds everything inside a Linux Docker container (it can't be built natively on macOS).

## Building (`build.sh` + `docker/Dockerfile`)

```
./build.sh all                 # rootfs, then kernel (default)
./build.sh rootfs              # -> output/<br config name>/rootfs.tar
./build.sh kernel              # -> output/kernel/{vmlinux,vmlinux.gz,modules-<rel>.tar.gz}
./build.sh kernel-menuconfig   # interactive; writes .config back to kernel_6.9.0/kernel_6.9_m68k_amiga_cfg
./build.sh rootfs-menuconfig   # interactive; writes .config back to the selected Buildroot config
./build.sh shell | image | clean
BR_CONFIG=buildroot_2024.05/buildroot_cfg_202405_pre_xorg ./build.sh rootfs
```

- On the host, the script builds the image and runs itself again inside the container as user `builder`. The repo is mounted at `/repo`. Sources, build trees and the download cache go in the named volume `amiga-linux-work` at `/work`. Don't move builds onto the macOS bind mount, because it is case-insensitive and slow.
- The Buildroot version is read from the config header (`# Buildroot X Configuration`), and its tarball is fetched automatically. Each config gets its own output directory, `/work/br-<config name>`. Files from `<buildroot dir>/package/` are rsynced into the Buildroot tree.
- The kernel is compiled with the **Buildroot toolchain** of the selected config (`make toolchain` runs if needed), not with a distro cross-gcc. Buildroot's GCC defaults to `-mcpu=68040`, so `kernel_make` passes `KCFLAGS/KAFLAGS=-m68020` whenever the kernel config enables 68020/68030. Without it, the 030 MMU inline asm (`pmove %psr`, `ptestr`, `ploadr`) fails to assemble. Kernel builds are out-of-tree (`O=/work/linux-build`). The pristine `linux-6.9` tree is re-extracted and re-patched whenever `sellen_mods_and_fixes.patch` changes.
- The repo configs are copied in and run through `olddefconfig` on every build. They are only written back by the `*-menuconfig` commands.

## How the pieces map onto upstream trees

### Kernel (`kernel_6.9.0/`)
- The files under `kernel_6.9.0/` (`drivers/...`, `arch/m68k/include/asm/...`, `include/linux/mfd/...`) go at the **same relative paths** in a Linux 6.9 source tree. They are new files, not diffs.
- `sellen_mods_and_fixes.patch` is a git-style diff against the kernel root (apply with `git apply` / `patch -p1`). It contains:
  - Kconfig/Makefile hooks for the Warp core and drivers (`MFD_CSWARP`, `PATA_CSWARP`, `AMIWARPNET`, `FB_AMIGA_WARP`). The driver `.c` files are useless without this patch. To change it, apply it to pristine 6.9 copies of the touched files in a scratch git repo, edit there, and regenerate it with `git diff`.
  - CS-Lab Zorro IDs (`ZORRO_MANUF_CSLAB` = 0x1400, and the products DDR3/VRAM/CTRL/XROM) in `include/uapi/linux/zorro_ids.h` and `drivers/zorro/zorro.ids`.
  - Unrelated fixes: inverted button polarity in `amimouse.c`, and a `depth:` option plus debug printks in `amifb.c`.
- `kernel_6.9_m68k_amiga_cfg` is the `.config` to use (`ARCH=m68k`, `CONFIG_LOCALVERSION="-amiga"`). It enables `MFD_CSWARP` and the Warp drivers built-in and disables the stock `FB_AMIGA`. Because `olddefconfig` silently turns a new, unset Kconfig symbol off, add any new symbol to this file explicitly.
- If you add or rename a driver, update both the source file here and the Kconfig/Makefile hunks in the patch.

### Warp driver architecture
The drivers target Warp firmware package **3.000+** (the state before this rework is git tag `before_v3`). `kernel_6.9.0/WARP_DRIVERS.md` lists the kernel command-line options, the debug mask bits and the sysfs files.
- **`drivers/mfd/cswarp-core.c`** (`MFD_CSWARP`) is the only driver that binds the Zorro **CTRL** board. It is the Linux counterpart of AmigaOS `cswarp.library`. It owns:
  - the register window, mapped with `z_ioremap`;
  - the one 4 KB DPRAM command/reply frame, behind `frame_lock` (a mutex: exchanges sleep);
  - the shared INT2 (`IRQ_AMIGA_PORTS`) handler. It clears the CR handshake bits (MR_ARM/MP_68K), then the async-unit doorbells (`IRQ_PEND` → clear `IRQ_IF` → dispatch).

  At probe it checks the DPREG id (`0x80570001`) and the firmware version from `dpcmdGetDiag`, quiesces every INT2 source, then adds the MFD children `cswarp-net`, `cswarp-ata` and `cswarp-fb`. Its API is in `include/linux/mfd/cswarp.h`:
  - `cswarp_frame_lock`/`_exchange`/`_unlock` and `cswarp_xfer`;
  - `cswarp_unit_register`;
  - the `cswarp_dbg()` macro, driven by the `cswarp_core.dbg` bitmask.

  After an exchange times out, the core marks the frame broken: a late reply from the ARM must never land in the next caller's frame.
- **`amiwarpnet.c`** registers two netdevs (`eth%d`): if 0 = WiFi, if 1 = USB ethernet. All board access runs on one ordered workqueue, because exchanges sleep while `ndo_start_xmit` cannot:
  - TX frames are queued per interface and sent round-robin;
  - the RX doorbells (`AUNIT_ETH0/1`) trigger a drain with `dpcmdEthIfReceive(ETH_IF_ANY)`;
  - link state and MAC come from `dpcmdEthIfGetInfo`, polled every second.

  The link worker only `rtnl_trylock()`s, because `ndo_stop` flushes the workqueue under RTNL.
- **`pata_cswarp.c`** handles the native IDE at CTRL+0x6000, with a 4-byte stride and ctl at +0x1018. It uses polled PIO with no IRQ. It refuses to bind unless the diag reply says native IDE mode: in Gayle-emulation mode the window isn't decoded.
- **`amiwarpfb.c`** gets the VRAM Zorro board (24 MB used) plus the core's registers.
  - **Mode-set order:** program the pixel clock first (`amiwarp_vidclk.c`, which reprograms the video MMCM through DRP, 25–148.5 MHz). Only touch PIXC-block registers once the MMCM is locked; otherwise the bus cycle never ends.
  - **Blitter addresses** are VRAM *physical* addresses: the hardware uses bits 24:4.
  - **Sync polarity** comes from `var->sync`.
  - On remove/shutdown it restores the native 108.125 MHz scandoubler output.
- **`amiwarp_vidclk.c`** is a bit-exact port of `csWarpAmigaSoftware/common/mmcm_drp.c` + `vidclk_hw.c`. Keep it in sync with them: if those change, re-port and compare outputs over the whole clock range.
- **Register access:** Warp registers are big-endian 32-bit words. Use `z_readl`/`z_writel`; `readl`/`writel` are little-endian on m68k.
- **Headers:** `asm/cswarpdefs.h` (register offsets) and `asm/cswarpamicommdata.h` (the protocol subset) mirror `csWarpAmigaSoftware/common/WarpDefs.h`, `WarpRegisters.h` and `WarpAmiCommData.h`. They are a wire contract: enum values are explicit, structs use `#pragma pack(2)`, and `cswarp-core.c` `static_assert`s the offsets.
- **Reference sources:** `~/workprojects/csAmigaWarp` (STM32 firmware `Arm/csArmedWarp`, FPGA `hdl/`) and `~/workprojects/csWarpAmigaSoftware` (AmigaOS drivers, shared headers in `common/`). Don't modify them from this repo's sessions unless explicitly asked.

### Buildroot (`buildroot_2024.02/`, `buildroot_2024.05/`)
- `buildroot_cfg_*` are full Buildroot `.config` files: m68k 68040, glibc, GCC 13, shared libs, tar rootfs. `BR2_LINUX_KERNEL` is **not** set, because the kernel is built separately from the tree above.
- `buildroot_2024.05/buildroot_cfg_20240705` is the newest (with Xorg and x11vnc). `_pre_xorg` is the same before X was added. The 2024.02 `noshadow`/`shadowlib` variants differ mainly in `BR2_PACKAGE_SHADOW`, hdparm/lsscsi and the issue string.
- Paths into this repo are written as `$(AMIGA_LINUX_REPO)/...`, for example `BR2_ROOTFS_OVERLAY="$(AMIGA_LINUX_REPO)/overlay"`. Buildroot expands this from the environment, and `build.sh` exports it. Keep using this form, not absolute paths.
- The 2024.02 configs reference `buildroot_2024.02/amiga_users_table.txt`, which is not in the repo yet.
- `buildroot_2024.05/package/x11vnc/0003-Fix-mutex-declarations.patch` goes into Buildroot's `package/x11vnc/` (next to its existing patches). It turns mutex definitions in a header into `static` ones, which avoids multiple-definition link errors with modern GCC.

### Rootfs overlay (`overlay/`)
Copied over the target rootfs by Buildroot. It contains `etc/X11/xorg.conf` (fbdev + shadow, input autodetection off), shell `.profile`s for `root` and user `andy`, and a demo web page in `var/www/`.

## Notes
- This is a cross-compile target (m68k). Nothing can be run on the dev machine. Verify driver changes with `./build.sh kernel`.
- `.DS_Store` files are macOS noise and shouldn't be committed.
