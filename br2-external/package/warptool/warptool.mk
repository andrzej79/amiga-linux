################################################################################
#
# warptool
#
################################################################################

WARPTOOL_VERSION = 0.1
WARPTOOL_SITE = $(BR2_EXTERNAL_WARP_PATH)/../warptool
WARPTOOL_SITE_METHOD = local
WARPTOOL_LICENSE = GPL-2.0

define WARPTOOL_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -std=gnu11 -Wall -Wextra \
		-Wno-unused-parameter -o $(@D)/warptool $(@D)/warptool.c
endef

define WARPTOOL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/warptool $(TARGET_DIR)/usr/bin/warptool
endef

$(eval $(generic-package))
