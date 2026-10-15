################################################################################
#
# fbdoom
#
################################################################################

FBDOOM_VERSION = 17280163bc95e5d954d2efaa0633489b763b4cd1
FBDOOM_SITE = $(call github,maximevince,fbDOOM,$(FBDOOM_VERSION))
# the shareware IWAD, from Debian's package (unmodified doom1.wad 1.9)
FBDOOM_WAD_DEB = doom-wad-shareware_1.9.fixed-5_all.deb
FBDOOM_EXTRA_DOWNLOADS = \
	http://deb.debian.org/debian/pool/non-free/d/doom-wad-shareware/$(FBDOOM_WAD_DEB)
FBDOOM_LICENSE = GPL-2.0+ (engine), id Software shareware license (doom1.wad)
FBDOOM_LICENSE_FILES = wad/usr/share/doc/doom-wad-shareware/copyright

define FBDOOM_EXTRACT_WAD
	mkdir -p $(@D)/wad
	cd $(@D)/wad && $(TARGET_AR) x $(FBDOOM_DL_DIR)/$(FBDOOM_WAD_DEB) data.tar.xz && \
		$(XZCAT) data.tar.xz | $(TAR) -xf - \
			./usr/share/games/doom/doom1.wad \
			./usr/share/doc/doom-wad-shareware/copyright
endef
FBDOOM_POST_EXTRACT_HOOKS += FBDOOM_EXTRACT_WAD

# -O2 (the measured setting) after the -O3 of TARGET_CFLAGS.  Not PIE:
# faster on m68k, and see 0003 for the rest of the speed work.
define FBDOOM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/fbdoom NOSDL=1 \
		CC="$(TARGET_CC)" CROSS_COMPILE="$(TARGET_CROSS)" \
		CFLAGS="$(TARGET_CFLAGS) -O2 -fno-pie -Wall -Wno-unused -DNORMALUNIX -DLINUX" \
		LDFLAGS="$(TARGET_LDFLAGS) -no-pie -Wl,--gc-sections"
endef

define FBDOOM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fbdoom/fbdoom $(TARGET_DIR)/usr/bin/fbdoom
	$(INSTALL) -D -m 0644 $(@D)/wad/usr/share/games/doom/doom1.wad \
		$(TARGET_DIR)/usr/share/games/doom/doom1.wad
	$(INSTALL) -D -m 0755 $(FBDOOM_PKGDIR)/doom $(TARGET_DIR)/usr/bin/doom
endef

$(eval $(generic-package))
