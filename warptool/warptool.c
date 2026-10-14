// SPDX-License-Identifier: GPL-2.0
/*
 * warptool - CS-Lab Warp board status and settings, the Linux counterpart of
 * AmigaOS WarpTool.
 *
 * Same keywords as WarpTool (case-insensitive, KEY=value or KEY value), run
 * in WarpTool's order: setters before the views that show them, so one
 * command line can change a setting and show the result.
 *
 * Everything goes through the kernel's cswarp-ctl device:
 *   /sys/bus/platform/devices/cswarp-ctl/{board,l2,audio}/...
 * so the board is never touched behind the drivers' backs.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define CTL_DIR		"/sys/bus/platform/devices/cswarp-ctl"
#define LABEL_LEN	24

static bool color;

#define C_VAL	(color ? "\033[32m" : "")
#define C_UNIT	(color ? "\033[33m\033[1m" : "")
#define C_HEAD	(color ? "\033[1m\033[32m" : "")
#define C_WARN	(color ? "\033[1m\033[31m" : "")
#define C_OFF	(color ? "\033[0m" : "")

/* ------------------------------------------------------------------------ */
/* sysfs                                                                    */
/* ------------------------------------------------------------------------ */

/* Read a whole file; NULL (errno set) on failure.  The caller frees it. */
static char *read_file(const char *rel)
{
	char path[256];
	char *buf;
	size_t n;
	FILE *f;

	snprintf(path, sizeof(path), CTL_DIR "/%s", rel);
	f = fopen(path, "r");
	if (!f)
		return NULL;
	buf = calloc(1, 8192);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	n = fread(buf, 1, 8191, f);
	if (ferror(f)) {
		int e = errno;

		fclose(f);
		free(buf);
		errno = e ? e : EIO;
		return NULL;
	}
	buf[n] = 0;
	fclose(f);
	return buf;
}

/* Write a value; 0 or an errno */
static int write_file(const char *rel, const char *val)
{
	char path[256];
	FILE *f;
	int err = 0;

	snprintf(path, sizeof(path), CTL_DIR "/%s", rel);
	f = fopen(path, "w");
	if (!f)
		return errno;
	if (fputs(val, f) == EOF)
		err = errno;
	if (fclose(f) == EOF && !err)
		err = errno;
	return err;
}

/* The value of "key value" in a sysfs listing, or NULL; copied to out */
static const char *kv(const char *text, const char *key, char *out, size_t len)
{
	size_t kl = strlen(key);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, key, kl) && p[kl] == ' ') {
			const char *v = p + kl + 1, *e = strchr(v, '\n');
			size_t n = e ? (size_t)(e - v) : strlen(v);

			if (n >= len)
				n = len - 1;
			memcpy(out, v, n);
			out[n] = 0;
			return out;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return NULL;
}

static long long kv_ll(const char *text, const char *key, long long dflt)
{
	char v[64];

	return kv(text, key, v, sizeof(v)) ? strtoll(v, NULL, 0) : dflt;
}

static bool have_ctl(void)
{
	if (access(CTL_DIR "/board/diag", R_OK) == 0)
		return true;
	fprintf(stderr, "warptool: no Warp board control device (%s)\n"
		"The kernel needs the cswarp-ctl driver (CONFIG_MFD_CSWARP).\n", CTL_DIR);
	return false;
}

/* ------------------------------------------------------------------------ */
/* Output                                                                   */
/* ------------------------------------------------------------------------ */

/* " Label........ : value unit", as WarpTool prints its diagnostic rows */
static void row(const char *label, const char *unit, const char *fmt, ...)
{
	va_list ap;
	int n;

	n = printf(" %s", label) - 1;
	for (; n < LABEL_LEN; n++)
		putchar('.');
	printf(" : %s", C_VAL);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("%s", C_OFF);
	if (unit && *unit)
		printf(" %s%s%s", C_UNIT, unit, C_OFF);
	putchar('\n');
}

static void banner(const char *title)
{
	printf("----------------------------------------------------\n");
	printf("%s%s%s\n", C_HEAD, title, C_OFF);
	printf("----------------------------------------------------\n");
}

/* m-units (x1000) as a decimal with the given number of fraction digits */
static void milli_str(char *buf, size_t len, long long m, int digits)
{
	long long div = digits == 1 ? 100 : digits == 2 ? 10 : 1;
	long long v = (m < 0 ? m - div / 2 : m + div / 2) / div;
	long long scale = digits == 1 ? 10 : digits == 2 ? 100 : 1000;
	long long a = v < 0 ? -v : v;

	snprintf(buf, len, "%s%lld.%0*lld", v < 0 ? "-" : "", a / scale, digits, a % scale);
}

static void fail(const char *what, int err)
{
	fprintf(stderr, "warptool: %s: %s\n", what, strerror(err));
}

/* ------------------------------------------------------------------------ */
/* Diag                                                                     */
/* ------------------------------------------------------------------------ */

static const char *usb_status_str(long long s)
{
	static const char *const names[] = {
		"No Device", "HID Device", "Mass Storage", "Other Device", "USB HUB",
	};

	return s >= 0 && s < 5 ? names[s] : "Other Device";
}

static const char *rssi_quality(long rssi)
{
	if (rssi >= -60)
		return "excellent";
	if (rssi >= -70)
		return "good";
	if (rssi >= -80)
		return "fair";
	return "poor";
}

static int cmd_diag(bool show_pass)
{
	char v[160], num[32];
	char *d = read_file("board/diag");
	int i;

	if (!d) {
		fail("cannot read the board's diagnostic data", errno);
		return 20;
	}
	banner("    CS-Lab s.c. Warp Accelerator Diagnostic Data");

	row("Model name", "", "%s", kv(d, "board", v, sizeof(v)) ? v : "?");
	row("Hardware version", "", "%04llX", kv_ll(d, "hw_version", 0));
	if (kv(d, "fw_version", v, sizeof(v))) {
		char *sp = strchr(v, ' ');

		if (sp)
			*sp = 0;
		row("Firmware version", "", "%04llX (%s)", strtoll(sp ? sp + 1 : "0", NULL, 0), v);
	}
	row("Selected Kickstart", "", "slot[%lld]", kv_ll(d, "kickstart_slot", 0));
	milli_str(num, sizeof(num), kv_ll(d, "cpu_clock_hz", 0) / 1000, 1);
	row("MC680x0 clk freq.", "MHz", "%s", num);
	milli_str(num, sizeof(num), kv_ll(d, "cpu_clock_phase_mdeg", 0), 1);
	row("MC680x0 clk phase", "deg", "%s", num);
	milli_str(num, sizeof(num), kv_ll(d, "cpu_temp_internal_mc", 0), 1);
	row("MC680x0 int. temp.", "\xc2\xb0" "C", "%s", num);
	milli_str(num, sizeof(num), kv_ll(d, "cpu_temp_ntc_mc", 0), 1);
	row("MC680x0 ext. temp.", "\xc2\xb0" "C", "%s", num);
	milli_str(num, sizeof(num), kv_ll(d, "arm_temp_mc", 0), 1);
	row("ARM STM32H7 temp.", "\xc2\xb0" "C", "%s", num);
	row("Artix FPGA temp.", "\xc2\xb0" "C", "%lld", kv_ll(d, "fpga_temp_mc", 0) / 1000);
	milli_str(num, sizeof(num), kv_ll(d, "vcc5v_mv", 0), 2);
	row("5V supply", "V", "%s", num);
	milli_str(num, sizeof(num), kv_ll(d, "vbatt_mv", 0), 2);
	row("Backup batt. voltage", "V", "%s", num);
	for (i = 0; i < 2; i++) {
		char key[16], label[32];

		snprintf(key, sizeof(key), "usb%d_status", i);
		snprintf(label, sizeof(label), "USB Host[%d] status", i);
		row(label, "", "%s", usb_status_str(kv_ll(d, key, 3)));
	}
	kv(d, "ide_mode", v, sizeof(v));
	row("IDE mode", "", "%s", !strcmp(v, "native") ? "native (pata_cswarp)" : "emulation (Gayle)");
	row("IDE timings", "", "%s", kv(d, "ide_timings", v, sizeof(v)) ? v : "?");
	row("Fan max RPM temperature", "\xc2\xb0" "C", "%lld", kv_ll(d, "fan_max_temp_c", 0));
	row("Fan min PWM duty", "%", "%lld", kv_ll(d, "fan_min_pwm_pct", 0));
	row("Fan current output", "%", "%lld", kv_ll(d, "fan_output_pct", 0));
	row("WiFi SSID", "", "%s", kv(d, "wifi_ssid", v, sizeof(v)) ? v : "");
	if (show_pass) {
		char *p = read_file("board/wifi_password");

		if (p) {
			p[strcspn(p, "\n")] = 0;
			row("WiFi password", "", "%s", p);
			free(p);
		} else {
			row("WiFi password", "", "(%s)", strerror(errno));
		}
	} else {
		row("WiFi password", "", "******");
	}
	if (kv(d, "wifi_state", v, sizeof(v))) {
		char *sp = strchr(v, ' ');

		if (sp)
			sp[1] = toupper((unsigned char)sp[1]);
		row("WiFi state", "", "%s", sp ? sp + 1 : v);
	}
	if (kv(d, "wifi_rssi_dbm", v, sizeof(v)) && strcmp(v, "n/a")) {
		long r = strtol(v, NULL, 10);

		row("WiFi sig. level", "", "%ld dBm (%s), channel %lld", r, rssi_quality(r),
		    kv_ll(d, "wifi_channel", 0));
	} else {
		row("WiFi sig. level", "", "n/a");
	}
	row("Video output mode", "", "%s", kv(d, "video_output", v, sizeof(v)) ? v : "n/a");
	if (kv(d, "l2_early_ack", v, sizeof(v))) {
		char *sp = strchr(v, ' ');

		if (sp) {
			*sp = 0;
			row("L2 early read ack", "", "%s (%s)", v,
			    !strcmp(sp + 1, "default") ? "board default" : "set by user");
		} else {
			row("L2 early read ack", "", "%s", v);
		}
	}
	putchar('\n');
	free(d);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* CPUFreqHz                                                                */
/* ------------------------------------------------------------------------ */

/*
 * Plain Hz, or with a k / M suffix and an optional "Hz": "105500000",
 * "105.5M", "105.5MHz", "50000k".  Integer arithmetic, so "105.5M" is
 * exactly 105500000.  A fraction without a unit is refused - a fractional
 * Hz is always a forgotten 'M'.  As WarpTool's parseFreqHz().
 */
static bool parse_freq_hz(const char *s, unsigned long *hz)
{
	unsigned long ip = 0, mult = 1, val, scale;
	const char *frac = NULL;
	int digits = 0;

	while (isdigit((unsigned char)*s)) {
		if (ip > 100000000UL)
			return false;
		ip = ip * 10 + (unsigned long)(*s++ - '0');
		digits++;
	}
	if (*s == '.') {
		frac = ++s;
		while (isdigit((unsigned char)*s)) {
			s++;
			digits++;
		}
	}
	if (!digits)
		return false;
	if (*s == 'k' || *s == 'K') {
		mult = 1000;
		s++;
	} else if (*s == 'm' || *s == 'M') {
		mult = 1000000;
		s++;
	}
	if (!strncasecmp(s, "hz", 2))
		s += 2;
	if (*s)
		return false;
	if (frac && mult == 1 && isdigit((unsigned char)*frac))
		return false;
	if (ip > 1000000000UL / mult)
		return false;
	val = ip * mult;
	if (frac)
		for (scale = mult / 10; scale && isdigit((unsigned char)*frac); scale /= 10, frac++)
			val += (unsigned long)(*frac - '0') * scale;
	*hz = val;
	return true;
}

static int cmd_cpu_freq(const char *arg)
{
	unsigned long hz, min, max;
	long long type, now;
	char *d, val[32], num[32];
	int err;

	if (!parse_freq_hz(arg, &hz)) {
		fprintf(stderr, "Invalid CPUFreqHz '%s'. Use Hz, or a k / M suffix, e.g. 105500000 or 105.5M.\n",
			arg);
		return 5;
	}
	d = read_file("board/diag");
	if (!d) {
		fail("cannot read the board's diagnostic data", errno);
		return 20;
	}
	type = kv_ll(d, "board_type", 0);
	free(d);
	switch (type) {
	case 2: case 5: case 8: case 11:		/* 540, 1240, 7040, CDTV40 */
		min = 25000000;
		max = 60000000;
		break;
	case 3: case 6: case 9: case 12: case 13:	/* 560, 1260, 7060, CDTV60, 1260X */
		min = 33000000;
		max = 120000000;
		break;
	default:
		fprintf(stderr, "The clock of this board type (%lld) cannot be set.\n", type);
		return 5;
	}
	if (hz < min)
		hz = min;
	if (hz > max)
		hz = max;
	milli_str(num, sizeof(num), (long long)hz / 1000, 1);
	printf("Setting MC680x0 clock to: %s MHz\n", num);
	snprintf(val, sizeof(val), "%lu\n", hz);
	err = write_file("board/cpu_clock_hz", val);
	if (err) {
		fail("setting the clock failed", err);
		return 20;
	}
	d = read_file("board/cpu_clock_hz");
	if (d) {
		now = strtoll(d, NULL, 10);
		milli_str(num, sizeof(num), now / 1000, 2);
		printf("MC680x0 clock is now: %s MHz\n", num);
		free(d);
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* L2                                                                       */
/* ------------------------------------------------------------------------ */

static void show_policy(void)
{
	char *p = read_file("l2/policy");

	if (!p) {
		printf("L2 replacement policy: n/a (this FPGA image has no policy switch)\n");
		return;
	}
	printf("L2 replacement policy: %s\n", !strncmp(p, "lru", 3) ? "LRU" : "legacy round-robin");
	free(p);
}

static void show_early_ack(void)
{
	char *p = read_file("l2/early_ack");
	char *sp;

	if (!p) {
		printf("L2 early read ack: n/a\n");
		return;
	}
	p[strcspn(p, "\n")] = 0;
	sp = strchr(p, ' ');
	if (sp) {
		*sp = 0;
		printf("L2 early read ack: %s (%s)\n", p, !strcmp(sp + 1, "default") ? "board default" : "set by user");
	} else {
		printf("L2 early read ack: %s\n", p);
	}
	free(p);
}

static int cmd_l2_policy(const char *arg)
{
	char val[16];
	int err;

	if (!strcasecmp(arg, "lru"))
		strcpy(val, "lru\n");
	else if (!strcasecmp(arg, "legacy"))
		strcpy(val, "legacy\n");
	else {
		fprintf(stderr, "Unknown L2 policy '%s'. Use 'lru' or 'legacy'.\n", arg);
		return 5;
	}
	err = write_file("l2/policy", val);
	if (err) {
		fail(err == ENOENT ? "this FPGA image has no L2 policy switch" : "setting the L2 policy failed", err);
		return 20;
	}
	show_policy();
	printf("L2 cache flushed, counters cleared.\n");
	return 0;
}

static int cmd_l2_early_ack(const char *arg)
{
	char val[16];
	int err;

	if (strcasecmp(arg, "on") && strcasecmp(arg, "off") && strcasecmp(arg, "default")) {
		fprintf(stderr, "Unknown L2EarlyAck value '%s'. Use 'on', 'off' or 'default'.\n", arg);
		return 5;
	}
	snprintf(val, sizeof(val), "%s\n", arg);
	for (char *c = val; *c; c++)
		*c = tolower((unsigned char)*c);
	err = write_file("l2/early_ack", val);
	if (err == EOPNOTSUPP) {
		printf("L2 early read ack: this FPGA core has no such switch, update the board firmware.\n");
		return 5;
	}
	if (err == ENOENT) {
		printf("L2 early read ack: board firmware too old, update it first.\n");
		return 5;
	}
	if (err) {
		fail("setting L2 early read ack failed", err);
		return 20;
	}
	show_early_ack();
	return 0;
}

static int cmd_l2_info(void)
{
	char *d = read_file("board/diag");
	long long kb, ways, line;

	if (!d) {
		fail("cannot read the board's diagnostic data", errno);
		return 20;
	}
	kb = kv_ll(d, "l2_size_kb", 0);
	ways = kv_ll(d, "l2_ways", 0);
	line = kv_ll(d, "l2_line_bytes", 0);
	free(d);
	if (!kb || !ways)
		printf("L2 cache: n/a\n");
	else if (!line)
		printf("L2 cache: %lldkB/%lld way assoc.\n", kb, ways);
	else
		printf("L2 cache: %lldkB/%lld way assoc./%lld B lines\n", kb, ways, line);
	show_policy();
	show_early_ack();
	return 0;
}

/* part/total as x.y% */
static void pct(char *buf, size_t len, unsigned long long part, unsigned long long total)
{
	unsigned long long pm = total ? (part * 1000 + total / 2) / total : 0;

	snprintf(buf, len, "%llu.%llu%%", pm / 10, pm % 10);
}

static int cmd_l2_stats(bool reset)
{
	char *s = read_file("l2/stats");
	unsigned long long acc, hit, miss, wb;
	char a[32], b[32], c[32];

	if (!s) {
		fail(errno == ENOENT ? "this FPGA image has no L2 counters" : "cannot read the L2 counters",
		     errno);
		return 20;
	}
	acc = kv_ll(s, "accesses", 0);
	hit = kv_ll(s, "hits", 0);
	miss = kv_ll(s, "misses", 0);
	wb = kv_ll(s, "writebacks", 0);
	free(s);
	pct(a, sizeof(a), hit, acc);
	pct(b, sizeof(b), miss, acc);
	pct(c, sizeof(c), wb, acc);
	row("L2 accesses", "", "%llu", acc);
	row("L2 hits", "", "%llu (%s)", hit, a);
	row("L2 misses", "", "%llu (%s)", miss, b);
	row("L2 write-backs", "", "%llu (%s)", wb, c);
	if (hit + miss > acc + 1)
		printf("Note: the counters wrapped since they were last cleared.\n");
	if (reset) {
		int err = write_file("l2/stats", "1\n");

		if (err) {
			fail("clearing the L2 counters failed", err);
			return 20;
		}
		printf("L2 counters cleared.\n");
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Audio                                                                    */
/* ------------------------------------------------------------------------ */

struct channel {
	const char *keyword;	/* WarpTool's */
	const char *file;	/* in audio/ */
	const char *name;
};

static const struct channel channels[] = {
	{ "volMaster",	"master",	"Master" },
	{ "volAmiga",	"paula",	"Paula" },
	{ "volAhi",	"ahi",		"AHI" },
	{ "volMhi",	"mhi",		"MHI" },
	{ "volLineOut",	"line_out",	"Line Out" },
	{ "volSpeaker",	"speaker",	"Speaker" },
};

/* "channel <name> <dB> <min> <max> <muted>" from audio/state */
static bool channel_state(const char *state, const char *file, int *db, int *min, int *max, int *mute)
{
	char key[32], v[64];

	snprintf(key, sizeof(key), "channel %s", file);
	if (!kv(state, key, v, sizeof(v)))
		return false;
	return sscanf(v, "%d %d %d %d", db, min, max, mute) == 4;
}

static bool audio_ready(void)
{
	if (access(CTL_DIR "/audio/state", R_OK) == 0)
		return true;
	printf("Audio levels need board firmware 30.54 or newer: update the board's firmware package.\n");
	return false;
}

/* "-12", "+3", "-12dB", "0 db", or "mute" / "off" - as WarpTool */
static int cmd_volume(const struct channel *ch, const char *arg)
{
	int db, min = 0, max = 0, mute, cur;
	char val[16], *end, *state;
	long v;
	int err;

	if (!audio_ready())
		return 20;
	state = read_file("audio/state");
	if (state) {
		channel_state(state, ch->file, &cur, &min, &max, &mute);
		free(state);
	}
	while (isspace((unsigned char)*arg))
		arg++;
	if (!strcasecmp(arg, "mute") || !strcasecmp(arg, "off")) {
		snprintf(val, sizeof(val), "mute\n");
		db = 0;
	} else {
		v = strtol(arg, &end, 10);
		while (isspace((unsigned char)*end))
			end++;
		if (end == arg || (*end && strcasecmp(end, "dB"))) {
			printf("%s: '%s' is not a level - give dB (%d..%d) or MUTE.\n", ch->name, arg, min, max);
			return 10;
		}
		db = (int)v;
		if (db < min || db > max) {
			printf("%s: %d dB is out of range - levels are in dB, %d..%d, or MUTE.\n",
			       ch->name, db, min, max);
			return 10;
		}
		snprintf(val, sizeof(val), "%d\n", db);
	}
	{
		char rel[32];

		snprintf(rel, sizeof(rel), "audio/%s", ch->file);
		err = write_file(rel, val);
	}
	if (err == ERANGE) {
		printf("%s: %d dB is out of range (%d..%d dB).\n", ch->name, db, min, max);
		return 10;
	}
	if (err) {
		printf("%s: the board refused the level (%s).\n", ch->name, strerror(err));
		return 10;
	}
	if (!strncmp(val, "mute", 4)) {
		printf("%s muted.\n", ch->name);
	} else {
		printf("%s set to %d dB.\n", ch->name, db);
		if (!strcmp(ch->file, "line_out") && db > 0)
			printf("Note: above 0 dB the line output boosts a full-scale signal into clipping.\n"
			       "Set the level with volMaster and leave volLineOut at 0.\n");
	}
	return 0;
}

static int cmd_audio(void)
{
	static const char *const order[] = { "paula", "ahi", "mhi", "master", "line_out", "speaker" };
	static const char *const names[] = { "Paula", "AHI", "MHI", "Master", "Line Out", "Speaker" };
	char *s, v[64], tmp[48], range[32];
	int db, min, max, mute, line_db = 0, line_mute = 1;
	size_t i;

	if (!audio_ready())
		return 20;
	s = read_file("audio/state");
	if (!s) {
		fail("the board did not answer the audio query", errno);
		return 20;
	}
	banner("       Warp Audio Mixer");
	for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		if (!channel_state(s, order[i], &db, &min, &max, &mute))
			continue;
		if (mute)
			snprintf(tmp, sizeof(tmp), "muted (%d dB)", db);
		else
			snprintf(tmp, sizeof(tmp), "%d dB", db);
		snprintf(range, sizeof(range), "range %d..%d dB", min, max);
		row(names[i], "", "%-16s %s", tmp, range);
		if (!strcmp(order[i], "line_out")) {
			line_db = db;
			line_mute = mute;
		}
	}
	putchar('\n');
	row("Codec", "", "%s", kv(s, "codec", v, sizeof(v)) && !strcmp(v, "ok") ? "OK" : "NOT RESPONDING");
	row("I2C errors", "", "%lld", kv_ll(s, "codec_i2c_errors", 0));
	row("Sample rate", "Hz", "%lld", kv_ll(s, "sample_rate_hz", 0));
	row("Clipped", "", "%lld samples since boot", kv_ll(s, "clipped_samples", 0));
	row("SAI errors", "", "%lld", kv_ll(s, "sai_errors", 0));
	if (line_db > 0 && !line_mute)
		printf("\n%sLine Out is above 0 dB: a full-scale signal clips. Use volLineOut=0.%s\n", C_WARN, C_OFF);
	free(s);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Arguments                                                                */
/* ------------------------------------------------------------------------ */

enum arg_id {
	A_DIAG, A_WIFISHOWPASS, A_CPUFREQ,
	A_VOLMASTER, A_VOLAMIGA, A_VOLAHI, A_VOLMHI, A_VOLLINEOUT, A_VOLSPEAKER, A_AUDIO,
	A_L2POLICY, A_L2EARLYACK, A_L2INFO, A_L2STATS, A_L2STATSRESET, A_HELP,
	A_COUNT
};

static const struct {
	const char *name;
	const char *alias;
	bool value;		/* /K: takes a value */
} args_def[A_COUNT] = {
	[A_DIAG]	= { "Diag", NULL, false },
	[A_WIFISHOWPASS] = { "WiFiShowPass", NULL, false },
	[A_CPUFREQ]	= { "CPUFreqHz", "CKHZ", true },
	[A_VOLMASTER]	= { "volMaster", NULL, true },
	[A_VOLAMIGA]	= { "volAmiga", NULL, true },
	[A_VOLAHI]	= { "volAhi", NULL, true },
	[A_VOLMHI]	= { "volMhi", NULL, true },
	[A_VOLLINEOUT]	= { "volLineOut", NULL, true },
	[A_VOLSPEAKER]	= { "volSpeaker", NULL, true },
	[A_AUDIO]	= { "Audio", NULL, false },
	[A_L2POLICY]	= { "L2Policy", NULL, true },
	[A_L2EARLYACK]	= { "L2EarlyAck", NULL, true },
	[A_L2INFO]	= { "L2Info", NULL, false },
	[A_L2STATS]	= { "L2Stats", NULL, false },
	[A_L2STATSRESET] = { "L2StatsReset", NULL, false },
	[A_HELP]	= { "help", "?", false },
};

static void usage(void)
{
	printf("Usage: warptool [KEYWORD[=value] ...]   (keywords are case-insensitive)\n\n"
	       "  Diag                  board diagnostic data (the default)\n"
	       "  WiFiShowPass          with Diag: show the WiFi password (root)\n"
	       "  CPUFreqHz=<f>         set the 68k clock: Hz, or 105.5M, 50000k\n"
	       "  L2Info                L2 cache geometry, policy, early read ack\n"
	       "  L2Policy=lru|legacy   L2 replacement policy (flushes the cache)\n"
	       "  L2EarlyAck=on|off|default\n"
	       "  L2Stats [L2StatsReset] L2 hit/miss counters (and clear them)\n"
	       "  Audio                 the audio mixer and codec\n"
	       "  volMaster= volAmiga= volAhi= volMhi= volLineOut= volSpeaker=\n"
	       "                        a level in dB (\"-12\", \"+3dB\") or MUTE\n\n"
	       "Setters run before the views: 'warptool volMaster=-6 Audio' shows the new level.\n"
	       "Everything goes through " CTL_DIR ".\n");
}

int main(int argc, char **argv)
{
	const char *val[A_COUNT] = { NULL };
	bool set[A_COUNT] = { false };
	bool any = false;
	int i, rc = 0, r;

	color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL;

	for (i = 1; i < argc; i++) {
		char *a = argv[i], *eq;
		size_t n;
		int k;

		while (*a == '-')	/* accept --diag, -h as well */
			a++;
		if (!strcasecmp(a, "h"))
			a = "help";
		eq = strchr(a, '=');
		n = eq ? (size_t)(eq - a) : strlen(a);
		for (k = 0; k < A_COUNT; k++)
			if ((strlen(args_def[k].name) == n && !strncasecmp(a, args_def[k].name, n)) ||
			    (args_def[k].alias && strlen(args_def[k].alias) == n &&
			     !strncasecmp(a, args_def[k].alias, n)))
				break;
		if (k == A_COUNT) {
			fprintf(stderr, "warptool: unknown keyword '%s' (warptool help)\n", argv[i]);
			return 10;
		}
		if (args_def[k].value) {
			if (eq) {
				val[k] = eq + 1;
			} else if (i + 1 < argc) {
				val[k] = argv[++i];
			} else {
				fprintf(stderr, "warptool: %s needs a value\n", args_def[k].name);
				return 10;
			}
		} else if (eq) {
			fprintf(stderr, "warptool: %s takes no value\n", args_def[k].name);
			return 10;
		}
		set[k] = true;
		if (k != A_WIFISHOWPASS)
			any = true;
	}

	if (set[A_HELP]) {
		usage();
		return 0;
	}
	if (!any)
		set[A_DIAG] = true;
	if (!have_ctl())
		return 20;

	/* WarpTool's run order */
	if (set[A_CPUFREQ] && (r = cmd_cpu_freq(val[A_CPUFREQ])) > rc)
		rc = r;
	for (i = A_VOLMASTER; i <= A_VOLSPEAKER; i++)
		if (set[i] && (r = cmd_volume(&channels[i - A_VOLMASTER], val[i])) > rc)
			rc = r;
	if (set[A_AUDIO] && (r = cmd_audio()) > rc)
		rc = r;
	if (set[A_L2POLICY] && (r = cmd_l2_policy(val[A_L2POLICY])) > rc)
		rc = r;
	if (set[A_L2EARLYACK] && (r = cmd_l2_early_ack(val[A_L2EARLYACK])) > rc)
		rc = r;
	if (set[A_L2INFO] && (r = cmd_l2_info()) > rc)
		rc = r;
	if ((set[A_L2STATS] || set[A_L2STATSRESET]) && (r = cmd_l2_stats(set[A_L2STATSRESET])) > rc)
		rc = r;
	if (set[A_DIAG] && (r = cmd_diag(set[A_WIFISHOWPASS])) > rc)
		rc = r;
	return rc;
}
