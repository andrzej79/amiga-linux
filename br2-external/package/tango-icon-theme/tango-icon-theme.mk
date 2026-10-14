################################################################################
#
# tango-icon-theme
#
################################################################################

TANGO_ICON_THEME_VERSION = 0.8.90
TANGO_ICON_THEME_SOURCE = tango-icon-theme-$(TANGO_ICON_THEME_VERSION).tar.bz2
TANGO_ICON_THEME_SITE = http://tango.freedesktop.org/releases
TANGO_ICON_THEME_LICENSE = Public Domain
TANGO_ICON_THEME_LICENSE_FILES = COPYING

# The PNG sizes are in the tarball; its configure only renders more from
# the SVGs (rsvg) and makes the index, which comes from here instead.
define TANGO_ICON_THEME_INSTALL_TARGET_CMDS
	for size in 16x16 22x22 32x32; do \
		for ctx in $(@D)/$$size/*/; do \
			ctx=$$(basename $$ctx); \
			$(INSTALL) -d $(TARGET_DIR)/usr/share/icons/Tango/$$size/$$ctx; \
			cp -a $(@D)/$$size/$$ctx/*.png \
				$(TARGET_DIR)/usr/share/icons/Tango/$$size/$$ctx/ 2>/dev/null || true; \
		done; \
	done
	$(INSTALL) -m 0644 $(TANGO_ICON_THEME_PKGDIR)/index.theme \
		$(TARGET_DIR)/usr/share/icons/Tango/index.theme
endef

$(eval $(generic-package))
