include $(sort $(wildcard $(BR2_EXTERNAL_WARP_PATH)/package/*/*.mk))

# coreutils: gnulib switches it to a 64-bit time_t on its own (year2038),
# but the rest of this system uses 32-bit time, and on m68k glibc's struct
# utmpx follows _TIME_BITS - who/users then misread the utmp records
# dropbear and systemd write (garbage login times).  coreutils marks 64-bit
# time as required, so --disable-year2038 is not enough: preset the probe's
# cache result instead.  Appended here, after Buildroot's coreutils.mk;
# _CONF_ENV is only expanded at configure time.
COREUTILS_CONF_ENV += ac_cv_sys_year2038_opts="none needed"
