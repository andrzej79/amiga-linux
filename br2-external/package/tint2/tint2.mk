################################################################################
#
# tint2
#
################################################################################

TINT2_VERSION = 17.0.2
TINT2_SITE = https://gitlab.com/o9000/tint2/-/archive/$(TINT2_VERSION)
TINT2_LICENSE = GPL-2.0
TINT2_LICENSE_FILES = COPYING
TINT2_DEPENDENCIES = host-pkgconf cairo pango libglib2 imlib2 \
	xlib_libX11 xlib_libXcomposite xlib_libXdamage xlib_libXext \
	xlib_libXinerama xlib_libXrandr xlib_libXrender

TINT2_CONF_OPTS = \
	-DENABLE_TINT2CONF=OFF \
	-DENABLE_BATTERY=OFF \
	-DENABLE_EXTRA_THEMES=OFF \
	-DENABLE_RSVG=OFF \
	-DENABLE_SN=OFF \
	-DENABLE_UEVENT=OFF

$(eval $(cmake-package))
