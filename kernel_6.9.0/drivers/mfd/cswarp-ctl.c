// SPDX-License-Identifier: GPL-2.0
/*
 * CS-Lab Warp turbo board - board status and settings
 *
 * Copyright (c) 2026 CS-Lab s.c.
 *		http://www.cs-lab.eu
 *
 * The Linux side of what AmigaOS WarpTool does through cswarp.library: the
 * board's diagnostic data, the 68k clock, the FPGA L2 cache and the audio
 * mixer, as files on the cswarp-ctl platform device, plus a hwmon device for
 * the temperatures, voltages and fan.  warptool (userspace) reads and writes
 * these files and nothing else.
 *
 *   board/diag		  everything dpcmdGetDiag reports, "key value" lines
 *   board/cpu_clock_hz	  68k clock; writing reprograms the board's PLL
 *   board/wifi_password  the stored WiFi password (root only)
 *   l2/size_kb, ways, line_bytes
 *   l2/policy		  lru | legacy (SysCfg; writing also flushes the L2)
 *   l2/early_ack	  on | off | default (the board stores it)
 *   l2/stats		  the four L2 counters; any write zeroes them
 *   audio/master, paula, ahi, mhi, line_out, speaker
 *			  "<dB>" or "<dB> mute"; write a dB value, "mute" or "unmute"
 *   audio/state	  codec state and every channel with its range
 *
 * The diag reply is kept for half a second, so reading several files does
 * not cost an exchange each.  Floats arrive as raw IEEE-754 words and are
 * converted with integer arithmetic.
 */

#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mfd/cswarp.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include <asm/cswarpamicommdata.h>
#include <asm/cswarpdefs.h>
#include <asm/delay.h>

#define DRV_NAME	"cswarp-ctl"

#define CTL_DIAG_MAX_AGE	(HZ / 2)
#define CTL_CLK_SETTLE_MS	50	/* PLL relock before the clock is read back */

/* cswarp.library's limits, by CPU */
#define CTL_CLK_68040_MIN_HZ	25000000
#define CTL_CLK_68040_MAX_HZ	60000000
#define CTL_CLK_68060_MIN_HZ	33000000
#define CTL_CLK_68060_MAX_HZ	120000000

static_assert(sizeof(DprCmdSetCPUClkConfig) == 14);
static_assert(sizeof(DprCmdL2EarlyAck) == 6);
static_assert(sizeof(DprCmdAudioSetChannel) == 8);
static_assert(offsetof(DprRplAudioState, st.ch) == 52);
static_assert(sizeof(DprRplAudioState) == 84);
static_assert(offsetof(DprRplDiagMsg, l2EarlyAckMode) == 408);

struct cswarp_ctl {
	struct device *dev;
	struct cswarp_core *wc;
	struct mutex lock;		/* the diag copy, and settings changes */
	DprRplDiagMsg *diag;
	unsigned long diag_time;	/* jiffies of the copy; valid if diag_ok */
	bool diag_ok;
};

/* ------------------------------------------------------------------------ */
/* IEEE-754 single precision, without the FPU                               */
/* ------------------------------------------------------------------------ */

/* f * scale, rounded; NaN and infinities read as 0 */
static s64 ctl_f32_scaled(u32 f, u32 scale)
{
	int exp = (f >> 23) & 0xff;
	s64 v;
	int sh;

	if (exp == 0 || exp == 0xff)
		return 0;
	v = (s64)((f & 0x7fffff) | 0x800000) * scale;
	sh = exp - 150;			/* value = mantissa * 2^(exp - 127 - 23) */
	if (sh >= 0)
		v = sh > 20 ? S64_MAX : v << sh;
	else
		v = -sh > 62 ? 0 : (v + (1LL << (-sh - 1))) >> -sh;
	return (f & 0x80000000) ? -v : v;
}

/* num / den as a float, rounded to nearest; num, den > 0 and well below 2^40 */
static u32 ctl_ratio_f32(u64 num, u64 den)
{
	int k = 0;
	u64 m;

	if (!num)
		return 0;
	while (num < (den << 23)) {
		num <<= 1;
		k++;
	}
	while (num >= (den << 24)) {
		den <<= 1;
		k--;
	}
	m = div64_u64(num + den / 2, den);
	if (m >= (1 << 24)) {
		m >>= 1;
		k--;
	}
	/* value = m * 2^-k, m in [2^23, 2^24) */
	return ((u32)(127 + 23 - k) << 23) | (u32)(m & 0x7fffff);
}

/* ------------------------------------------------------------------------ */
/* Board access                                                             */
/* ------------------------------------------------------------------------ */

/* The diag reply, refreshed if older than CTL_DIAG_MAX_AGE.  ctl->lock held. */
static const DprRplDiagMsg *ctl_diag(struct cswarp_ctl *ctl, bool fresh)
{
	DprCmdHeader cmd = { .cmd = dpcmdGetDiag };
	int err;

	lockdep_assert_held(&ctl->lock);
	if (ctl->diag_ok && !fresh && time_before(jiffies, ctl->diag_time + CTL_DIAG_MAX_AGE))
		return ctl->diag;
	err = cswarp_xfer(ctl->wc, &cmd, sizeof(cmd), ctl->diag, sizeof(*ctl->diag), dprplDiagFrame);
	if (err) {
		ctl->diag_ok = false;
		return ERR_PTR(err);
	}
	ctl->diag->boardName[sizeof(ctl->diag->boardName) - 1] = 0;
	ctl->diag->wifiSSID[sizeof(ctl->diag->wifiSSID) - 1] = 0;
	ctl->diag->wifiPass[sizeof(ctl->diag->wifiPass) - 1] = 0;
	ctl->diag_time = jiffies;
	ctl->diag_ok = true;
	return ctl->diag;
}

/* FPGA die temperature, XADC code to m°C (cswarp.library's formula) */
static long ctl_fpga_temp_mc(struct cswarp_ctl *ctl)
{
	u32 x = cswarp_readl(ctl->wc, WARP_ARTIX_TEMP) & 0xfff;

	return (long)div_u64((u64)x * 503975000ULL, 4096) / 1000 - 273150;
}

/* SysCfg capability ids count up from SYSCFG_ID_ORIGINAL (0x012345xx) */
static bool ctl_syscfg_at_least(struct cswarp_ctl *ctl, u32 id)
{
	u32 have = ctl->wc->syscfg_id;

	return (have & 0xffffff00) == (SYSCFG_ID_ORIGINAL & 0xffffff00) && have >= id;
}

static bool ctl_has_l2cfg(struct cswarp_ctl *ctl)
{
	return ctl_syscfg_at_least(ctl, SYSCFG_ID_L2CFG);
}

static bool ctl_has_l2stats(struct cswarp_ctl *ctl)
{
	return ctl_syscfg_at_least(ctl, SYSCFG_ID_L2STATS);
}

static bool ctl_has_audio(struct cswarp_ctl *ctl)
{
	return cswarp_fw_at_least(ctl->wc, WARP_FMW_VER_MIN_AUDIOMIX);
}

/* 68040 or 68060 board: the clock range; false for anything else */
static bool ctl_clk_range(u8 type, u32 *min, u32 *max)
{
	switch (type) {
	case WARP_BOARD_540:
	case WARP_BOARD_1240:
	case WARP_BOARD_7040:
	case WARP_BOARD_CDTV40:
		*min = CTL_CLK_68040_MIN_HZ;
		*max = CTL_CLK_68040_MAX_HZ;
		return true;
	case WARP_BOARD_560:
	case WARP_BOARD_1260:
	case WARP_BOARD_1260X:
	case WARP_BOARD_7060:
	case WARP_BOARD_CDTV60:
		*min = CTL_CLK_68060_MIN_HZ;
		*max = CTL_CLK_68060_MAX_HZ;
		return true;
	default:
		return false;
	}
}

/* ------------------------------------------------------------------------ */
/* board/                                                                   */
/* ------------------------------------------------------------------------ */

static const char *const usb_status_names[] = {
	[USBDevNone]		= "none",
	[USBDevHID]		= "HID",
	[USBDevMassStorage]	= "mass storage",
	[USBDevOther]		= "other",
	[USBDevHub]		= "hub",
};

static const char *const wifi_state_names[] = {
	[wifiUnknown]		= "unknown",
	[wifiStarted]		= "started",
	[wifiStopped]		= "stopped",
	[wifiConnected]		= "connected",
	[wifiDisconnected]	= "disconnected",
};

static const char *ctl_name(const char *const *names, size_t n, u32 v)
{
	return v < n && names[v] ? names[v] : "unknown";
}

static ssize_t diag_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	const DprRplDiagMsg *d;
	char ver[16];
	int len, i;

	mutex_lock(&ctl->lock);
	d = ctl_diag(ctl, false);
	if (IS_ERR(d)) {
		mutex_unlock(&ctl->lock);
		return PTR_ERR(d);
	}
	cswarp_fw_ver_str(d->fmwVer, ver, sizeof(ver));
	len = sysfs_emit(buf,
			 "board %s\n"
			 "board_type %u %s\n"
			 "hw_version 0x%04x\n"
			 "fw_version %s 0x%04x\n"
			 "kickstart_slot %u\n"
			 "cpu_clock_hz %lld\n"
			 "cpu_clock_phase_mdeg %lld\n"
			 "cpu_temp_internal_mc %lld\n"
			 "cpu_temp_ntc_mc %lld\n"
			 "arm_temp_mc %lld\n"
			 "fpga_temp_mc %ld\n"
			 "vcc5v_mv %lld\n"
			 "vbatt_mv %lld\n",
			 d->boardName, d->warpBoardType, cswarp_board_type_name(d->warpBoardType),
			 d->hwVer, ver, d->fmwVer, d->kickstartNr,
			 ctl_f32_scaled(d->cpuFreqMHz, 1000000),
			 ctl_f32_scaled(d->cpuClkPhaseShiftDeg, 1000),
			 ctl_f32_scaled(d->t60internal, 1000), ctl_f32_scaled(d->t60ntc, 1000),
			 ctl_f32_scaled(d->tArm, 1000), ctl_fpga_temp_mc(ctl),
			 ctl_f32_scaled(d->vcc5v, 1000), ctl_f32_scaled(d->vBatt, 1000));
	for (i = 0; i < AMICOMM_USB_HOSTS; i++)
		len += sysfs_emit_at(buf, len, "usb%d_status %u %s\n", i, d->usbDevStatus[i],
				     ctl_name(usb_status_names, ARRAY_SIZE(usb_status_names),
					      d->usbDevStatus[i]));
	len += sysfs_emit_at(buf, len,
			     "ide_mode %s\n"
			     "ide_timings %u %u %u %u %u\n"
			     "fan_max_temp_c %d\n"
			     "fan_min_pwm_pct %d\n"
			     "fan_output_pct %u\n"
			     "wifi_ssid %s\n"
			     "wifi_state %u %s\n",
			     d->nativeIdeEnabled ? "native" : "emulation",
			     d->IDE_IOR_as, d->IDE_IOR_ng, d->IDE_IOW_as, d->IDE_IOW_ng, d->IDE_ACK_as,
			     d->tempRegCpu, d->tempRegMinPwm, d->fanPercent, d->wifiSSID,
			     d->wifiState, ctl_name(wifi_state_names, ARRAY_SIZE(wifi_state_names),
						    d->wifiState));
	if (d->wifiRssiValid)
		len += sysfs_emit_at(buf, len, "wifi_rssi_dbm %d\n", d->wifiRssi);
	else
		len += sysfs_emit_at(buf, len, "wifi_rssi_dbm n/a\n");
	len += sysfs_emit_at(buf, len,
			     "wifi_channel %u\n"
			     "video_output %s\n"
			     "l2_size_kb %u\n"
			     "l2_ways %u\n"
			     "l2_line_bytes %u\n",
			     d->wifiChannel, d->hdmiEnabled ? "HDMI" : "DVI",
			     d->l2CacheSizeKB, d->l2CacheWays, d->l2CacheLineBytes);
	if (!cswarp_fw_at_least(ctl->wc, WARP_FMW_VER_MIN_L2EARLYACK) || !L2EA_REG_CAPID(d->l2EarlyAckReg))
		len += sysfs_emit_at(buf, len, "l2_early_ack n/a\n");
	else
		len += sysfs_emit_at(buf, len, "l2_early_ack %s %s\n",
				     (d->l2EarlyAckReg & L2EA_REG_ENABLE) ? "on" : "off",
				     d->l2EarlyAckMode == L2EA_MODE_DEFAULT ? "default" : "user");
	mutex_unlock(&ctl->lock);
	return len;
}
static DEVICE_ATTR_RO(diag);

static ssize_t wifi_password_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	const DprRplDiagMsg *d;
	ssize_t len;

	mutex_lock(&ctl->lock);
	d = ctl_diag(ctl, false);
	len = IS_ERR(d) ? PTR_ERR(d) : sysfs_emit(buf, "%s\n", d->wifiPass);
	mutex_unlock(&ctl->lock);
	return len;
}
static DEVICE_ATTR_ADMIN_RO(wifi_password);

static ssize_t cpu_clock_hz_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	const DprRplDiagMsg *d;
	ssize_t len;

	mutex_lock(&ctl->lock);
	d = ctl_diag(ctl, false);
	len = IS_ERR(d) ? PTR_ERR(d) : sysfs_emit(buf, "%lld\n", ctl_f32_scaled(d->cpuFreqMHz, 1000000));
	mutex_unlock(&ctl->lock);
	return len;
}

/*
 * The board reprograms the PLL at once (and keeps the setting).  udelay()
 * counts loops calibrated at boot, so loops_per_jiffy follows the clock.
 */
static ssize_t cpu_clock_hz_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	DprCmdSetCPUClkConfig cmd = {
		.header.cmd = dpcmdSetCPUClkConfig,
		.cpuClkPhaseShiftDeg = 0,		/* 0.0f */
		.cpuClockPhaseShiftAuto = 1,
	};
	const DprRplDiagMsg *d;
	u32 hz, min, max;
	s64 old_hz, new_hz;
	int err;

	err = kstrtou32(buf, 0, &hz);
	if (err)
		return err;
	if (!ctl_clk_range(ctl->wc->board_type, &min, &max))
		return -EOPNOTSUPP;
	if (hz < min || hz > max)
		return -ERANGE;

	mutex_lock(&ctl->lock);
	d = ctl_diag(ctl, true);
	if (IS_ERR(d)) {
		err = PTR_ERR(d);
		goto out;
	}
	old_hz = ctl_f32_scaled(d->cpuFreqMHz, 1000000);

	cmd.cpuClockMHz = ctl_ratio_f32(hz, 1000000);
	err = cswarp_xfer(ctl->wc, &cmd, sizeof(cmd), NULL, 0, CSWARP_RPL_ANY);
	if (err)
		goto out;
	msleep(CTL_CLK_SETTLE_MS);

	d = ctl_diag(ctl, true);
	if (IS_ERR(d)) {
		err = PTR_ERR(d);
		goto out;
	}
	new_hz = ctl_f32_scaled(d->cpuFreqMHz, 1000000);
	if (old_hz > 0 && new_hz > 0 && new_hz != old_hz)
		loops_per_jiffy = div64_u64((u64)loops_per_jiffy * new_hz, old_hz);
	dev_info(dev, "68k clock %lld -> %lld Hz (asked %u), loops_per_jiffy %lu\n",
		 old_hz, new_hz, hz, loops_per_jiffy);
out:
	mutex_unlock(&ctl->lock);
	return err ? err : count;
}
static DEVICE_ATTR_RW(cpu_clock_hz);

static struct attribute *ctl_board_attrs[] = {
	&dev_attr_diag.attr,
	&dev_attr_wifi_password.attr,
	&dev_attr_cpu_clock_hz.attr,
	NULL
};

static const struct attribute_group ctl_board_group = {
	.name = "board",
	.attrs = ctl_board_attrs,
};

/* ------------------------------------------------------------------------ */
/* l2/                                                                      */
/* ------------------------------------------------------------------------ */

static ssize_t ctl_l2_geometry(struct device *dev, char *buf, int which)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	const DprRplDiagMsg *d;
	ssize_t len;

	mutex_lock(&ctl->lock);
	d = ctl_diag(ctl, false);
	if (IS_ERR(d))
		len = PTR_ERR(d);
	else
		len = sysfs_emit(buf, "%u\n", which == 0 ? d->l2CacheSizeKB :
				 which == 1 ? d->l2CacheWays : d->l2CacheLineBytes);
	mutex_unlock(&ctl->lock);
	return len;
}

static ssize_t size_kb_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return ctl_l2_geometry(dev, buf, 0);
}
static DEVICE_ATTR_RO(size_kb);

static ssize_t ways_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return ctl_l2_geometry(dev, buf, 1);
}
static DEVICE_ATTR_RO(ways);

static ssize_t line_bytes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return ctl_l2_geometry(dev, buf, 2);
}
static DEVICE_ATTR_RO(line_bytes);

static ssize_t policy_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	u32 cfg = cswarp_readl(ctl->wc, WARP_SYSCFG_L2CFG);

	return sysfs_emit(buf, "%s\n", (cfg & SYSCFG_L2CFG_LRU) ? "lru" : "legacy");
}

/* As WarpTool: set the policy and zero the counters in one write, then flush */
static ssize_t policy_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	u32 pol;

	if (sysfs_streq(buf, "lru"))
		pol = SYSCFG_L2CFG_LRU;
	else if (sysfs_streq(buf, "legacy"))
		pol = SYSCFG_L2CFG_LEGACY;
	else
		return -EINVAL;
	mutex_lock(&ctl->lock);
	cswarp_writel(ctl->wc, pol | SYSCFG_L2CFG_STATCLR, WARP_SYSCFG_L2CFG);
	cswarp_l2_flush(ctl->wc);
	mutex_unlock(&ctl->lock);
	return count;
}
static DEVICE_ATTR_RW(policy);

static ssize_t early_ack_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	const DprRplDiagMsg *d;
	ssize_t len;

	mutex_lock(&ctl->lock);
	d = ctl_diag(ctl, false);
	if (IS_ERR(d))
		len = PTR_ERR(d);
	else if (!L2EA_REG_CAPID(d->l2EarlyAckReg))
		len = sysfs_emit(buf, "n/a\n");
	else
		len = sysfs_emit(buf, "%s %s\n", (d->l2EarlyAckReg & L2EA_REG_ENABLE) ? "on" : "off",
				 d->l2EarlyAckMode == L2EA_MODE_DEFAULT ? "default" : "user");
	mutex_unlock(&ctl->lock);
	return len;
}

static ssize_t early_ack_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	DprCmdL2EarlyAck cmd = { .header.cmd = dpcmdSetL2EarlyAck };
	const DprRplDiagMsg *d;
	int err;

	if (sysfs_streq(buf, "on"))
		cmd.mode = L2EA_MODE_ON;
	else if (sysfs_streq(buf, "off"))
		cmd.mode = L2EA_MODE_OFF;
	else if (sysfs_streq(buf, "default"))
		cmd.mode = L2EA_MODE_DEFAULT;
	else
		return -EINVAL;

	mutex_lock(&ctl->lock);
	/* an FPGA core without the switch would take the command silently */
	d = ctl_diag(ctl, false);
	if (IS_ERR(d))
		err = PTR_ERR(d);
	else if (!L2EA_REG_CAPID(d->l2EarlyAckReg))
		err = -EOPNOTSUPP;
	else
		err = cswarp_xfer(ctl->wc, &cmd, sizeof(cmd), NULL, 0, CSWARP_RPL_ANY);
	ctl->diag_ok = false;
	mutex_unlock(&ctl->lock);
	return err ? err : count;
}
static DEVICE_ATTR_RW(early_ack);

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	u32 acc, hit, miss, wb;

	mutex_lock(&ctl->lock);
	acc = cswarp_readl(ctl->wc, WARP_SYSCFG_L2ACC);	/* latches all four */
	hit = cswarp_readl(ctl->wc, WARP_SYSCFG_L2HIT);
	miss = cswarp_readl(ctl->wc, WARP_SYSCFG_L2MISS);
	wb = cswarp_readl(ctl->wc, WARP_SYSCFG_L2WB);
	mutex_unlock(&ctl->lock);
	return sysfs_emit(buf, "accesses %u\nhits %u\nmisses %u\nwritebacks %u\n", acc, hit, miss, wb);
}

static ssize_t stats_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	u32 cfg;

	mutex_lock(&ctl->lock);
	cfg = cswarp_readl(ctl->wc, WARP_SYSCFG_L2CFG) & SYSCFG_L2CFG_MASK;
	cswarp_writel(ctl->wc, cfg | SYSCFG_L2CFG_STATCLR, WARP_SYSCFG_L2CFG);
	mutex_unlock(&ctl->lock);
	return count;
}
static DEVICE_ATTR_RW(stats);

static struct attribute *ctl_l2_attrs[] = {
	&dev_attr_size_kb.attr,
	&dev_attr_ways.attr,
	&dev_attr_line_bytes.attr,
	&dev_attr_policy.attr,
	&dev_attr_early_ack.attr,
	&dev_attr_stats.attr,
	NULL
};

/* Registers the FPGA image does not decode hang the bus: hide their files */
static umode_t ctl_l2_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(kobj_to_dev(kobj));

	if (attr == &dev_attr_policy.attr && !ctl_has_l2cfg(ctl))
		return 0;
	if (attr == &dev_attr_stats.attr && !ctl_has_l2stats(ctl))
		return 0;
	if (attr == &dev_attr_early_ack.attr &&
	    !cswarp_fw_at_least(ctl->wc, WARP_FMW_VER_MIN_L2EARLYACK))
		return 0;
	return attr->mode;
}

static const struct attribute_group ctl_l2_group = {
	.name = "l2",
	.attrs = ctl_l2_attrs,
	.is_visible = ctl_l2_visible,
};

/* ------------------------------------------------------------------------ */
/* audio/                                                                   */
/* ------------------------------------------------------------------------ */

static const char *const audio_channel_names[AUDCH_COUNT] = {
	[AUDCH_PAULA]	= "paula",
	[AUDCH_AHI]	= "ahi",
	[AUDCH_MHI]	= "mhi",
	[AUDCH_MASTER]	= "master",
	[AUDCH_LINEOUT]	= "line_out",
	[AUDCH_SPEAKER]	= "speaker",
};

/* Get the mixer state (set_ch < 0) or set one channel; ctl->lock held */
static int ctl_audio(struct cswarp_ctl *ctl, int set_ch, s8 db, u8 flags, DprRplAudioState *rpl)
{
	int err;

	if (set_ch < 0) {
		DprCmdAudioGetState cmd = { .header.cmd = dpcmdAudioGetState };

		err = cswarp_xfer(ctl->wc, &cmd, sizeof(cmd), rpl, sizeof(*rpl), dprplAudioState);
	} else {
		DprCmdAudioSetChannel cmd = {
			.header.cmd = dpcmdAudioSetChannel,
			.channel = set_ch,
			.gainDb = db,
			.flags = flags,
		};

		err = cswarp_xfer(ctl->wc, &cmd, sizeof(cmd), rpl, sizeof(*rpl), dprplAudioState);
	}
	if (err)
		return err;
	if (rpl->st.version < 1 || rpl->st.channelCount < AUDCH_COUNT)
		return -EPROTO;
	return 0;
}

struct ctl_audio_attr {
	struct device_attribute attr;
	int channel;
};

static ssize_t audio_channel_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	int ch = container_of(attr, struct ctl_audio_attr, attr)->channel;
	DprRplAudioState *rpl;
	ssize_t len;

	rpl = kzalloc(sizeof(*rpl), GFP_KERNEL);
	if (!rpl)
		return -ENOMEM;
	mutex_lock(&ctl->lock);
	len = ctl_audio(ctl, -1, 0, 0, rpl);
	mutex_unlock(&ctl->lock);
	if (!len)
		len = sysfs_emit(buf, "%d%s\n", rpl->st.ch[ch].gainDb,
				 (rpl->st.ch[ch].flags & AUDF_MUTE) ? " mute" : "");
	kfree(rpl);
	return len;
}

/*
 * "<dB>" sets the level and unmutes; "mute" keeps the level, so "unmute"
 * brings it back.  The board refuses levels out of the channel's range.
 */
static ssize_t audio_channel_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	int ch = container_of(attr, struct ctl_audio_attr, attr)->channel;
	DprRplAudioState *rpl;
	bool mute = false, keep = false;
	s8 db = 0;
	int err;

	if (sysfs_streq(buf, "mute"))
		mute = keep = true;
	else if (sysfs_streq(buf, "unmute"))
		keep = true;
	else if (kstrtos8(buf, 10, &db))
		return -EINVAL;

	rpl = kzalloc(sizeof(*rpl), GFP_KERNEL);
	if (!rpl)
		return -ENOMEM;
	mutex_lock(&ctl->lock);
	err = ctl_audio(ctl, -1, 0, 0, rpl);
	if (err)
		goto out;
	if (keep)
		db = rpl->st.ch[ch].gainDb;
	else if (db < rpl->st.ch[ch].minDb || db > rpl->st.ch[ch].maxDb) {
		err = -ERANGE;
		goto out;
	}
	err = ctl_audio(ctl, ch, db, mute ? AUDF_MUTE : 0, rpl);
	if (!err && rpl->st.result == AUDRES_RANGE)
		err = -ERANGE;
	else if (!err && rpl->st.result == AUDRES_CODEC)
		err = -EIO;
	else if (!err && rpl->st.result != AUDRES_OK)
		err = -EINVAL;
out:
	mutex_unlock(&ctl->lock);
	kfree(rpl);
	return err ? err : count;
}

#define CTL_AUDIO_ATTR(_name, _ch)						\
	static struct ctl_audio_attr ctl_audio_##_name = {			\
		.attr = __ATTR(_name, 0644, audio_channel_show, audio_channel_store), \
		.channel = _ch,							\
	}

CTL_AUDIO_ATTR(paula, AUDCH_PAULA);
CTL_AUDIO_ATTR(ahi, AUDCH_AHI);
CTL_AUDIO_ATTR(mhi, AUDCH_MHI);
CTL_AUDIO_ATTR(master, AUDCH_MASTER);
CTL_AUDIO_ATTR(line_out, AUDCH_LINEOUT);
CTL_AUDIO_ATTR(speaker, AUDCH_SPEAKER);

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	DprRplAudioState *rpl;
	const DprAudioState *st;
	ssize_t len;
	int i;

	rpl = kzalloc(sizeof(*rpl), GFP_KERNEL);
	if (!rpl)
		return -ENOMEM;
	mutex_lock(&ctl->lock);
	len = ctl_audio(ctl, -1, 0, 0, rpl);
	mutex_unlock(&ctl->lock);
	if (len)
		goto out;
	st = &rpl->st;
	len = sysfs_emit(buf,
			 "codec %s\n"
			 "codec_i2c_errors %u\n"
			 "sample_rate_hz %u\n"
			 "clipped_samples %u\n"
			 "sai_errors %u\n",
			 (st->codecFlags & AUDCODEC_F_INIT_OK) ? "ok" : "not-responding",
			 st->codecI2cErrors, st->sampleRate, st->clipCount, st->saiErrors);
	/* channel <name> <dB> <min> <max> <muted> */
	for (i = 0; i < AUDCH_COUNT; i++)
		len += sysfs_emit_at(buf, len, "channel %s %d %d %d %d\n", audio_channel_names[i],
				     st->ch[i].gainDb, st->ch[i].minDb, st->ch[i].maxDb,
				     !!(st->ch[i].flags & AUDF_MUTE));
out:
	kfree(rpl);
	return len;
}
static DEVICE_ATTR_RO(state);

static struct attribute *ctl_audio_attrs[] = {
	&ctl_audio_paula.attr.attr,
	&ctl_audio_ahi.attr.attr,
	&ctl_audio_mhi.attr.attr,
	&ctl_audio_master.attr.attr,
	&ctl_audio_line_out.attr.attr,
	&ctl_audio_speaker.attr.attr,
	&dev_attr_state.attr,
	NULL
};

static umode_t ctl_audio_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(kobj_to_dev(kobj));

	return ctl_has_audio(ctl) ? attr->mode : 0;
}

static const struct attribute_group ctl_audio_group = {
	.name = "audio",
	.attrs = ctl_audio_attrs,
	.is_visible = ctl_audio_visible,
};

static const struct attribute_group *ctl_groups[] = {
	&ctl_board_group,
	&ctl_l2_group,
	&ctl_audio_group,
	NULL
};

/* ------------------------------------------------------------------------ */
/* hwmon                                                                    */
/* ------------------------------------------------------------------------ */

#if IS_REACHABLE(CONFIG_HWMON)

enum { CTL_T_CPU_INT, CTL_T_CPU_NTC, CTL_T_ARM, CTL_T_FPGA };
enum { CTL_IN_5V, CTL_IN_VBATT };

static const char *const ctl_temp_labels[] = { "68k internal", "68k NTC", "ARM", "FPGA" };
static const char *const ctl_in_labels[] = { "+5V", "Vbatt" };

static umode_t ctl_hwmon_visible(const void *data, enum hwmon_sensor_types type, u32 attr, int ch)
{
	return 0444;
}

static int ctl_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int ch, long *val)
{
	struct cswarp_ctl *ctl = dev_get_drvdata(dev);
	const DprRplDiagMsg *d;
	int err = 0;

	if (type == hwmon_temp && ch == CTL_T_FPGA) {
		*val = ctl_fpga_temp_mc(ctl);
		return 0;
	}
	mutex_lock(&ctl->lock);
	d = ctl_diag(ctl, false);
	if (IS_ERR(d)) {
		err = PTR_ERR(d);
		goto out;
	}
	switch (type) {
	case hwmon_temp:
		*val = ctl_f32_scaled(ch == CTL_T_CPU_INT ? d->t60internal :
				      ch == CTL_T_CPU_NTC ? d->t60ntc : d->tArm, 1000);
		break;
	case hwmon_in:
		*val = ctl_f32_scaled(ch == CTL_IN_5V ? d->vcc5v : d->vBatt, 1000);
		break;
	case hwmon_pwm:		/* the fan's output, 0..255 */
		*val = DIV_ROUND_CLOSEST(min_t(u32, d->fanPercent, 100) * 255, 100);
		break;
	default:
		err = -EOPNOTSUPP;
	}
out:
	mutex_unlock(&ctl->lock);
	return err;
}

static int ctl_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				 int ch, const char **str)
{
	if (type == hwmon_temp)
		*str = ctl_temp_labels[ch];
	else if (type == hwmon_in)
		*str = ctl_in_labels[ch];
	else
		return -EOPNOTSUPP;
	return 0;
}

static const struct hwmon_ops ctl_hwmon_ops = {
	.is_visible = ctl_hwmon_visible,
	.read = ctl_hwmon_read,
	.read_string = ctl_hwmon_read_string,
};

static const struct hwmon_channel_info *const ctl_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_chip_info ctl_hwmon_chip = {
	.ops = &ctl_hwmon_ops,
	.info = ctl_hwmon_info,
};

static void ctl_hwmon_register(struct cswarp_ctl *ctl)
{
	struct device *hw;

	hw = devm_hwmon_device_register_with_info(ctl->dev, "warp", ctl, &ctl_hwmon_chip, NULL);
	if (IS_ERR(hw))
		dev_warn(ctl->dev, "no hwmon device (%ld)\n", PTR_ERR(hw));
}

#else
static void ctl_hwmon_register(struct cswarp_ctl *ctl) { }
#endif

/* ------------------------------------------------------------------------ */
/* Probe                                                                    */
/* ------------------------------------------------------------------------ */

static int ctl_probe(struct platform_device *pdev)
{
	struct cswarp_core *wc = cswarp_core_get(pdev);
	struct cswarp_ctl *ctl;

	if (!wc)
		return -ENODEV;
	ctl = devm_kzalloc(&pdev->dev, sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return -ENOMEM;
	ctl->diag = devm_kzalloc(&pdev->dev, sizeof(*ctl->diag), GFP_KERNEL);
	if (!ctl->diag)
		return -ENOMEM;
	ctl->dev = &pdev->dev;
	ctl->wc = wc;
	mutex_init(&ctl->lock);
	platform_set_drvdata(pdev, ctl);

	ctl_hwmon_register(ctl);
	cswarp_dbg(CSWARP_DBG_CORE, ctl->dev, "board controls: L2 policy %s, L2 stats %s, audio %s\n",
		   ctl_has_l2cfg(ctl) ? "yes" : "no", ctl_has_l2stats(ctl) ? "yes" : "no",
		   ctl_has_audio(ctl) ? "yes" : "no");
	return 0;
}

static struct platform_driver ctl_driver = {
	.probe		= ctl_probe,
	.driver		= {
		.name		= DRV_NAME,
		.dev_groups	= ctl_groups,
	},
};
module_platform_driver(ctl_driver);

MODULE_AUTHOR("Andrzej Rogozynski");
MODULE_DESCRIPTION("CS-Lab Warp board status and settings (sysfs, hwmon)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
