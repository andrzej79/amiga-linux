################################################################################
#
# jwm
#
################################################################################

JWM_VERSION = 2.4.6
JWM_SOURCE = jwm-$(JWM_VERSION).tar.xz
JWM_SITE = https://github.com/joewing/jwm/releases/download/v$(JWM_VERSION)
JWM_LICENSE = MIT
JWM_LICENSE_FILES = LICENSE
JWM_DEPENDENCIES = host-pkgconf xlib_libX11 xlib_libXext xlib_libXinerama \
	xlib_libXmu xlib_libXpm libpng

# core fonts only: anti-aliased text would be drawn in software on the Warp
JWM_CONF_OPTS = \
	--disable-xft \
	--disable-pango \
	--disable-cairo \
	--disable-rsvg \
	--disable-xrender \
	--disable-nls \
	--disable-rpath

ifeq ($(BR2_PACKAGE_JPEG),y)
JWM_DEPENDENCIES += jpeg
else
JWM_CONF_OPTS += --disable-jpeg
endif

$(eval $(autotools-package))
