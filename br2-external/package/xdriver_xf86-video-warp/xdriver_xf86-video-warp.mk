################################################################################
#
# xdriver_xf86-video-warp
#
################################################################################

XDRIVER_XF86_VIDEO_WARP_VERSION = 0.1
XDRIVER_XF86_VIDEO_WARP_SITE = $(BR2_EXTERNAL_WARP_PATH)/../xdriver
XDRIVER_XF86_VIDEO_WARP_SITE_METHOD = local
XDRIVER_XF86_VIDEO_WARP_LICENSE = MIT
XDRIVER_XF86_VIDEO_WARP_DEPENDENCIES = xserver_xorg-server host-pkgconf

define XDRIVER_XF86_VIDEO_WARP_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -fPIC -shared -fvisibility=hidden -Wall \
		-Wno-unused-parameter \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags xorg-server pixman-1) \
		-o $(@D)/warp_drv.so $(@D)/warp_drv.c
endef

define XDRIVER_XF86_VIDEO_WARP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/warp_drv.so \
		$(TARGET_DIR)/usr/lib/xorg/modules/drivers/warp_drv.so
endef

$(eval $(generic-package))
