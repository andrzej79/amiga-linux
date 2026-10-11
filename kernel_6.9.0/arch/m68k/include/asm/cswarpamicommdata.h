/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CS-Lab Warp turbo board - 68k <-> ARM dual-port RAM protocol.
 *
 * A subset of csWarpAmigaSoftware common/WarpAmiCommData.h (firmware 3.000),
 * limited to what the Linux drivers use.  The layouts are a wire contract with
 * the board firmware: keep every struct, offset and enum value identical to
 * that header.  Enum values are written out explicitly so a reordering there
 * cannot silently change them here; drivers/mfd/cswarp-core.c checks the
 * offsets at build time.
 *
 * The frame is big-endian on the wire (the ARM side byte-swaps), which is the
 * 68k's native order.
 */
#ifndef _ASM_M68K_CSWARPAMICOMMDATA_H
#define _ASM_M68K_CSWARPAMICOMMDATA_H

#include <linux/types.h>
#include <linux/bits.h>

/* ---- DPREG register file (at WARP_OFFSET_DPREG_CR) ---------------------- */
/*
 * Register idx lives at WARP_OFFSET_DPREG_CR + 4 * idx.  CR, IRQ_IF and IRQ_IE
 * take set/clear writes: bit 31 selects set (1) or clear (0), bits 30..0 say
 * which bits.  No read-modify-write is ever needed.
 */
#define DPREG_IDX_CR			0x00
#define DPREG_IDX_ID			0x01	/* read only: DPREG_ID_VALUE */
#define DPREG_IDX_IRQ_IF		0x02	/* ARM sets, 68k clears */
#define DPREG_IDX_IRQ_IE		0x03	/* 68k only */
#define DPREG_IDX_IRQ_PEND		0x04	/* read only: IRQ_IF & IRQ_IE */
#define DPREG_IDX_UNIT_STATUS(n)	(0x10 + (n))
#define DPREG_IDX_UNIT_COUNT(n)		(0x20 + (n))

#define DPREG_ID_VALUE			0x80570001UL
#define DPREG_SET			0x80000000UL
#define DPREG_CLR			0x00000000UL
#define DPREG_ALL_BITS			0x7fffffffUL

/* CR bits (register 0) */
#define DPREG_CR_SET			DPREG_SET
#define DPREG_CR_CLR			DPREG_CLR
#define DPREG_CR_IE_ARM			BIT(0)	/* ARM irq enable */
#define DPREG_CR_IE_68K			BIT(1)	/* 68k irq enable */
#define DPREG_CR_MP_ARM			BIT(2)	/* message pending for the ARM */
#define DPREG_CR_MP_68K			BIT(3)	/* reply pending for the 68k */
#define DPREG_CR_MR_ARM			BIT(4)	/* ARM has taken the message */
#define DPREG_CR_MR_68K			BIT(5)
/*
 * Bits 6..13 were the pre-23.00 doorbells (ethernet RX, AHI, floppy).  Nothing
 * sets them any more but the FPGA still ORs them onto INT2, so they must be
 * kept clear.
 */
#define DPREG_CR_LEGACY_DOORBELLS	0x00003fc0UL

/* Async units: unit n owns IRQ_IF/IRQ_IE bit n */
#define AUNIT_AHI			0
#define AUNIT_MHI			1
#define AUNIT_ETH0			2	/* WiFi (ETH_IF_WIFI) has RX frames */
#define AUNIT_ETH1			3	/* USB ethernet (ETH_IF_USB) has RX frames */
#define AUNIT_SD			4
#define AUNIT_USB0			5
#define AUNIT_USB1			6
#define AUNIT_CD			7
#define AUNIT_FLOPPY			8
#define AUNIT_JPEG			9
#define AUNIT_EXT			10
#define AUNIT_MAX			16

/* ---- Frame ---------------------------------------------------------------- */
#define DPR_FRAME_SIZE			4096

/* Ethernet */
#define ETH_MTU_AND_HDR_SIZE		(1500 + 14)
#define ETH_MAC_SIZE			6
#define ETH_IF_WIFI			0
#define ETH_IF_USB			1
#define ETH_IF_COUNT			2
#define ETH_IF_ANY			0xff	/* dpcmdEthIfReceive only */

/* Firmware version as reported in DprRplDiagMsg.fmwVer (release 30.54 = 0x3054) */
#define WARP_FMW_VER(maj, min, bld)	((((u32)(maj)) << 12) | (((u32)(min)) << 8) | ((u32)(bld)))
#define WARP_FMW_VER_DEVBUILD		WARP_FMW_VER(0, 9, 0)	/* no bootloader: accept */
#define WARP_FMW_VER_MIN_ETHIF		WARP_FMW_VER(2, 2, 0x67)
#define WARP_FMW_VER_MIN_ASYNC		WARP_FMW_VER(2, 3, 0x00)
#define WARP_FMW_VER_MIN_LINUX		WARP_FMW_VER(3, 0, 0x00)	/* release 3.000 */

/* Commands (68k -> ARM), DprCmd */
enum {
	dpcmdGetDiag			= 6,
	dpcmdSetIdeMode			= 7,
	dpcmdEthTransmit		= 23,	/* legacy, interface 0 only */
	dpcmdEthReceive			= 24,	/* legacy */
	dpcmdEthGetMACAddr		= 25,	/* legacy */
	dpcmdEthIfTransmit		= 47,	/* no reply */
	dpcmdEthIfReceive		= 48,	/* reply dprplEthIfReceive */
	dpcmdEthIfGetInfo		= 49,	/* reply dprplEthIfInfo */
};

/* Replies (ARM -> 68k), DprRpl */
enum {
	dprplNop			= 0,
	dprplDiagFrame			= 1,
	dprplEthReceive			= 10,
	dprplEthMACAddr			= 11,
	dprplEthIfReceive		= 23,
	dprplEthIfInfo			= 24,
};

#pragma pack(push, 2)

typedef struct {
	u32 cmd;
} DprCmdHeader;

typedef struct {
	u32 rpl;
} DprRplHeader;

/* dpcmdEthIfTransmit: 'reserved' puts packet[] at offset 8 */
typedef struct {
	DprCmdHeader header;
	u8  ifNr;			/* ETH_IF_*, never ETH_IF_ANY */
	u8  reserved;
	u16 pktSize;
	u8  packet[ETH_MTU_AND_HDR_SIZE];
} DprCmdEthIfSend;

/* dpcmdEthIfReceive / dpcmdEthIfGetInfo */
typedef struct {
	DprCmdHeader header;
	u8 ifNr;
	u8 reserved;
} DprCmdEthIf;

/* pktSize == 0: the queue named by the request is drained */
typedef struct {
	DprRplHeader header;
	u8  ifNr;
	u8  reserved;
	u16 pktSize;
	u8  packet[ETH_MTU_AND_HDR_SIZE];
} DprRplEthIfRecv;

/* present == 0: nothing behind this interface; mac[] is then meaningless */
typedef struct {
	DprRplHeader header;
	u8  ifNr;
	u8  present;
	u8  linkUp;
	u8  fullDuplex;
	u32 bps;			/* 0 while the link is down */
	u16 mtu;
	u8  mac[ETH_MAC_SIZE];
	u8  ifCount;
	u8  reserved;
} DprRplEthIfInfo;

#define AMICOMM_BOARDNAME_LEN		32
#define AMICOMM_USB_HOSTS		2
#define AMICOMM_WIFI_SSID_LEN		128
#define AMICOMM_WIFI_PASS_LEN		128

/* dprplDiagFrame.  The float fields are carried as raw IEEE-754 words. */
typedef struct {
	DprRplHeader header;
	u32 vcc5v;
	u32 vBatt;
	u32 t60ntc;
	u32 t60internal;
	u32 tArm;
	u32 hwVer;
	u32 fmwVer;
	u32 currentTurboLevel;		/* obsolete */
	u32 fanPercent;
	char boardName[AMICOMM_BOARDNAME_LEN];
	u32 kickstartNr;
	u32 usbDevStatus[AMICOMM_USB_HOSTS];
	char wifiSSID[AMICOMM_WIFI_SSID_LEN];
	char wifiPass[AMICOMM_WIFI_PASS_LEN];
	u32 nativeIdeEnabled;
	s32 tempRegCpu;
	s32 tempRegMinPwm;
	u8  IDE_IOR_as;
	u8  IDE_IOR_ng;
	u8  IDE_IOW_as;
	u8  IDE_IOW_ng;
	u8  IDE_ACK_as;
	s32 timeZoneCorrSecs;		/* obsolete */
	u8  warpBoardType;
	u32 wifiState;
	u32 cpuFreqMHz;			/* float */
	u32 cpuClkPhaseShiftDeg;	/* float */
	u32 l2CacheSizeKB;
	u32 l2CacheWays;
	u32 l2CacheLineBytes;
	u32 hdmiEnabled;
	u32 wifiRssiValid;
	s32 wifiRssi;
	u32 wifiChannel;
	u32 l2EarlyAckReg;
	u32 l2EarlyAckMode;
} DprRplDiagMsg;

#pragma pack(pop)

#endif /* _ASM_M68K_CSWARPAMICOMMDATA_H */
