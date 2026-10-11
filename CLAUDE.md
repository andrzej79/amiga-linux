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
- The files under `kernel_6.9.0/` (`drivers/...`, `arch/m68k/include/asm/...`) go at the **same relative paths** in a Linux 6.9 source tree. They are new files, not diffs.
- `sellen_mods_and_fixes.patch` is a git-style diff against the kernel root (apply with `git apply` / `patch -p1`). It contains:
  - Kconfig/Makefile hooks for the three Warp drivers (`PATA_CSWARP`, `AMIWARPNET`, `FB_AMIGA_WARP`). The driver `.c` files are useless without this patch.
  - CS-Lab Zorro IDs (`ZORRO_MANUF_CSLAB` = 0x1400, and the products DDR3/VRAM/CTRL/XROM) in `include/uapi/linux/zorro_ids.h` and `drivers/zorro/zorro.ids`.
  - Unrelated fixes: inverted button polarity in `amimouse.c`, and a `depth:` option plus debug printks in `amifb.c`.
- `kernel_6.9_m68k_amiga_cfg` is the `.config` to use (`ARCH=m68k`, `CONFIG_LOCALVERSION="-amiga"`). It enables the Warp drivers built-in and disables the stock `FB_AMIGA`.
- If you add or rename a driver, update both the source file here and the Kconfig/Makefile hunks in the patch.

### Warp driver architecture
All three drivers bind to the Warp's Zorro devices. Most go through the **CTRL** board (`ZORRO_PROD_CSLAB_WARP_CTRL`). Its register window is laid out by offsets in `asm/cswarpdefs.h` (`WARP_OFFSET_DPRAM`, `_DPREG_CR`, `_ATA`, `_QSDMA`, `_SYSCFG`, and the video register blocks `WarpRegs_mclk/pix/bclk`).
- `amiwarpfb.c`: fbdev driver. Probes the **VRAM** Zorro device and also looks up CTRL for registers. It supports the hardware blitter, pattern blitter, line draw, CLUT, sprite and the pixel clock modes enumerated in `cswarpdefs.h`. Options come from `video=amiwarpfb:...` (`fb_get_options`) or the `mode_option`/`depth_option` module params.
- `amiwarpnet.c`: Ethernet. The network itself is handled by the board's ARM side. The 68k talks to it through a **dual-port RAM mailbox**: it writes a `DprCmdFrame` at `ctrlBase | WARP_OFFSET_DPRAM`, signals via `DPREG_CR` bits and reads back a `DprRplFrame`. The IRQ is the shared `IRQ_AMIGA_PORTS`. The protocol structs and command enums are in `asm/cswarpamicommdata.h` (`#pragma pack(2)`). They must stay binary-compatible with the Warp firmware, so don't reorder or resize fields.
- `pata_cswarp.c`: libata driver for the on-board IDE at `WARP_OFFSET_ATA`. It needs `idemode=native` set in the Warp configuration.

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
