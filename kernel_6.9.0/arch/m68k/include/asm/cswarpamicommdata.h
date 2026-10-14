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
#define AUNIT_USB(n)			(AUNIT_USB0 + (n))

/* UNIT_STATUS[n]: [31:16] seq  [15:8] result (READY: chunk index)  [7:4] phase  [3:0] events */
#define USTAT_SEQ(s)			(((u32)(s) >> 16) & 0xffff)
#define USTAT_RESULT(s)			(((u32)(s) >> 8) & 0xff)
#define USTAT_CHUNK(s)			USTAT_RESULT(s)		/* UPHASE_READY only */
#define USTAT_PHASE(s)			(((u32)(s) >> 4) & 0xf)

#define UPHASE_IDLE			0
#define UPHASE_BUSY			1	/* accepted, device work in progress */
#define UPHASE_READY			2	/* a read chunk is staged / write chunk k-2 is done */
#define UPHASE_DONE			3	/* finished; UNIT_COUNT is bytes transferred */

/* Unit results (UNIT_STATUS when DONE, and every unit reply) */
#define URES_OK				0
#define URES_IOERR			1
#define URES_NOMEDIA			2
#define URES_WRPROT			3
#define URES_BADSECSZ			4
#define URES_ABORTED			5
#define URES_BADPARAM			6
#define URES_NODEV			7
#define URES_BUSY			8	/* an operation is already on this unit */
#define URES_BADSEQ			9
#define URES_NOTREADY			10
#define URES_NOMEM			11

/* ---- Frame ---------------------------------------------------------------- */
#define DPR_FRAME_SIZE			4096

/* Ethernet */
#define ETH_MTU_AND_HDR_SIZE		(1500 + 14)
#define ETH_MAC_SIZE			6
#define ETH_IF_WIFI			0
#define ETH_IF_USB			1
#define ETH_IF_COUNT			2
#define ETH_IF_ANY			0xff	/* dpcmdEthIfReceive only */

/* Disks */
#define DISK_MAX_DPRAM_TRANSFER		7
#define DISK_BLOCKSIZE			512
#define WARP_USB_DISK_MAX_UNITS		2

/*
 * Async unit operations (dpcmdUnitSubmit and friends).  One operation per unit;
 * see WarpAmiCommData.h for the full streaming protocol.
 */
#define UNIT_INLINE_MAX			(DISK_MAX_DPRAM_TRANSFER * DISK_BLOCKSIZE)	/* 3584 */
#define UOP_READ			1
#define UOP_WRITE			2
#define UOP_SCSI			3
#define UOP_WDATA			4	/* chunk k >= 1 of the UOP_WRITE with the same seq */
#define UNIT_WR_INLINE_CHUNK(blk)	((UNIT_INLINE_MAX / (blk)) * (blk))
#define UFETCH_DMA			0x01
#define UFETCH_RELEASE			0x02	/* the chunk's last fetch */
#define UNITCAPS_VERSION		3

/*
 * Chunk of a QSDMA stream of total bytes through staging halves of half bytes:
 * whole halves for a long operation, one cut in two when that still leaves
 * UNIT_SPLIT_MIN, so the device and the channel overlap.  Both sides compute it.
 */
#define UNIT_SPLIT_MIN			(16u * 1024u)
#define UNIT_HALF_OP(total, blk)	(((((total) / 2u) + (blk) - 1u) / (blk)) * (blk))
#define UNIT_STREAM_CHUNK(total, half, blk)						\
	(((total) >= 2u * (half)) ? (half)						\
	 : (UNIT_HALF_OP(total, blk) < UNIT_SPLIT_MIN) ? (((total) < (half)) ? (total) : (half)) \
	 : ((UNIT_HALF_OP(total, blk) < (half)) ? UNIT_HALF_OP(total, blk) : (half)))

/* Firmware version as reported in DprRplDiagMsg.fmwVer (release 30.54 = 0x3054) */
#define WARP_FMW_VER(maj, min, bld)	((((u32)(maj)) << 12) | (((u32)(min)) << 8) | ((u32)(bld)))
#define WARP_FMW_VER_DEVBUILD		WARP_FMW_VER(0, 9, 0)	/* no bootloader: accept */
#define WARP_FMW_VER_MIN_ETHIF		WARP_FMW_VER(2, 2, 0x67)
#define WARP_FMW_VER_MIN_ASYNC		WARP_FMW_VER(2, 3, 0x00)
#define WARP_FMW_VER_MIN_NETTLM		WARP_FMW_VER(2, 2, 0x88)

/*
 * dpcmdGetNetTelemetry: request { header, u32 flags }, reply { header, then
 * NETTLM_WORDS u32 words } - schema version NETTLM_VERSION, all fields 32 bit
 * (see DprNetTelemetry in WarpAmiCommData.h for the names and order).
 */
#define NETTLM_VERSION			8
#define NETTLM_FLAG_RESET		BIT(0)
#define NETTLM_WORDS			103
#define WARP_FMW_VER_MIN_LINUX		WARP_FMW_VER(3, 0, 0x00)	/* release 3.000 */
#define WARP_FMW_VER_MIN_L2EARLYACK	WARP_FMW_VER(3, 0, 0x22)	/* dpcmdSetL2EarlyAck, diag l2EarlyAck* */
#define WARP_FMW_VER_MIN_AUDIOMIX	WARP_FMW_VER(3, 0, 0x54)	/* dpcmdAudioGetState/SetChannel */

/* Commands (68k -> ARM), DprCmd */
enum {
	dpcmdGetDiag			= 6,
	dpcmdSetIdeMode			= 7,
	dpcmdSDGetInfo			= 17,	/* reply dprplSDGetInfo */
	dpcmdSetCPUClkConfig		= 36,	/* no reply; the PLL changes at once */
	dpcmdUSBDiskGetUnits		= 40,	/* reply dprplUSBDiskUnits */
	dpcmdEthTransmit		= 23,	/* legacy, interface 0 only */
	dpcmdEthReceive			= 24,	/* legacy */
	dpcmdEthGetMACAddr		= 25,	/* legacy */
	dpcmdEthIfTransmit		= 47,	/* no reply */
	dpcmdEthIfReceive		= 48,	/* reply dprplEthIfReceive */
	dpcmdEthIfGetInfo		= 49,	/* reply dprplEthIfInfo */
	dpcmdGetNetTelemetry		= 54,	/* reply dprplNetTelemetry, fw >= 22.88 */
	dpcmdUnitSubmit			= 61,	/* reply dprplUnitAck */
	dpcmdUnitFetch			= 62,	/* reply dprplUnitData */
	dpcmdUnitCancel			= 63,	/* reply dprplUnitAck */
	dpcmdUnitCaps			= 64,	/* reply dprplUnitCaps */
	dpcmdSetL2EarlyAck		= 71,	/* no reply, fw >= 30.22 */
	dpcmdAudioGetState		= 82,	/* reply dprplAudioState, fw >= 30.54 */
	dpcmdAudioSetChannel		= 83,	/* reply dprplAudioState, fw >= 30.54 */
};

/* Replies (ARM -> 68k), DprRpl */
enum {
	dprplNop			= 0,
	dprplDiagFrame			= 1,
	dprplSDGetInfo			= 4,
	dprplEthReceive			= 10,
	dprplEthMACAddr			= 11,
	dprplUSBDiskUnits		= 17,
	dprplEthIfReceive		= 23,
	dprplEthIfInfo			= 24,
	dprplNetTelemetry		= 28,
	dprplUnitAck			= 31,
	dprplUnitData			= 32,
	dprplUnitCaps			= 33,
	dprplAudioState			= 39,
};

/* dpcmdGetDiag: usbDevStatus[] */
enum {
	USBDevNone			= 0,
	USBDevHID			= 1,
	USBDevMassStorage		= 2,
	USBDevOther			= 3,
	USBDevHub			= 4,
};

/* dpcmdGetDiag: wifiState */
enum {
	wifiUnknown			= 0,
	wifiStarted			= 1,
	wifiStopped			= 2,
	wifiConnected			= 3,
	wifiDisconnected		= 4,
};

/* dpcmdSetL2EarlyAck mode, and the stored diag l2EarlyAckMode */
#define L2EA_MODE_DEFAULT		0
#define L2EA_MODE_OFF			1
#define L2EA_MODE_ON			2
/* diag l2EarlyAckReg: live FPGA readback; capability id 0 = no such switch */
#define L2EA_REG_ENABLE			0x00000001u
#define L2EA_REG_CAPID(r)		((u32)(r) >> 24)

/* The audio mixer (fw >= 30.54): every level a gain in whole dB */
#define AUDCH_PAULA			0	/* the Amiga's own audio, through the codec ADC */
#define AUDCH_AHI			1
#define AUDCH_MHI			2
#define AUDCH_MASTER			3
#define AUDCH_LINEOUT			4	/* codec headphone/line driver */
#define AUDCH_SPEAKER			5	/* codec speaker driver */
#define AUDCH_COUNT			6
#define AUDCH_MAX			8	/* slots in DprAudioState.ch[] */
#define AUDF_MUTE			0x01u
#define AUDGS_METERS			0x01u
#define AUDRES_OK			0
#define AUDRES_BADCHANNEL		1
#define AUDRES_RANGE			2	/* gain outside [minDb, maxDb]: refused */
#define AUDRES_CODEC			3	/* stored, but the codec did not take it */
#define AUDCODEC_F_INIT_OK		0x01u
#define AUDIOSTATE_VERSION		1

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

/* dpcmdUnitSubmit.  Every field is written on every call (the frame is never cleared). */
typedef struct {
	DprCmdHeader header;
	u8  unit;			/* AUNIT_SD .. AUNIT_FLOPPY */
	u8  op;				/* UOP_* */
	u8  dmaEnable;
	u8  dmaAlgn;
	u16 seq;			/* non-zero; echoed in every status and reply */
	u16 blockSize;
	u32 blockAddr;			/* UOP_WDATA: the chunk index */
	u32 totalBytes;			/* UOP_READ/UOP_WRITE: whole operation; UOP_WDATA: this chunk */
	u32 dataLen;			/* valid bytes in data[] */
	u8  data[UNIT_INLINE_MAX];
} DprCmdUnitSubmit;

typedef struct {
	DprCmdHeader header;
	u8  unit;
	u8  flags;			/* UFETCH_* */
	u8  dmaAlgn;
	u8  reserved;
	u16 seq;
	u16 chunk;			/* full chunk index */
	u32 offset;			/* byte offset within the chunk */
	u32 len;			/* at most UNIT_INLINE_MAX unless UFETCH_DMA */
} DprCmdUnitFetch;

/* seq 0 cancels whatever is there */
typedef struct {
	DprCmdHeader header;
	u8  unit;
	u8  reserved;
	u16 seq;
} DprCmdUnitCancel;

/* Ack for dpcmdUnitSubmit / dpcmdUnitCancel: only says whether the job was taken */
typedef struct {
	DprRplHeader header;
	u8  unit;
	u8  result;			/* URES_* */
	u16 seq;
	u32 chunkBytes;			/* UOP_READ / UOP_WRITE: the stream's chunk size */
} DprRplUnitAck;

typedef struct {
	DprRplHeader header;
	u8  unit;
	u8  result;
	u16 seq;
	u32 len;
	u8  data[UNIT_INLINE_MAX];
} DprRplUnitData;

typedef struct {
	DprRplHeader header;
	u32 version;			/* UNITCAPS_VERSION */
	u32 unitMask;			/* AUNIT_* bits this firmware implements */
	u32 stagingBytes[AUNIT_MAX];
	u32 chunkBytes[AUNIT_MAX];	/* staging half; 0 = none */
} DprRplUnitCaps;

/*
 * dprplSDGetInfo, leading fields only: the reply goes on with the card's CSD,
 * CID and bus information (AmiCommSD_CSD/_CID/_BusInfo), which Linux does not
 * read.
 */
typedef struct {
	DprRplHeader header;
	u8  cardInitialized;
	u32 state;
	u32 type;
	u32 blockNbr;
	u32 blockSize;
} DprRplSDGetInfoHead;

typedef struct {
	u8  present;			/* the slot holds a mounted medium */
	u8  identValid;			/* vendor/product/revision are filled in */
	u8  reserved[2];
	u32 blockNbr;
	u32 blockSize;
	u32 changeCnt;			/* bumps on every attach and detach of this unit */
	u32 vidPid;
	char serial[20];		/* NUL padded */
	char vendor[8];			/* INQUIRY strings: space padded, not NUL terminated */
	char product[16];
	char revision[4];
} DprUSBDiskUnit;

typedef struct {
	DprRplHeader header;
	u32 unitCount;
	DprUSBDiskUnit units[WARP_USB_DISK_MAX_UNITS];
} DprRplUSBDiskUnits;

/* dpcmdSetCPUClkConfig: the floats are raw IEEE-754 words */
typedef struct {
	DprCmdHeader header;
	u32 cpuClockMHz;		/* float */
	u32 cpuClkPhaseShiftDeg;	/* float, ignored when cpuClockPhaseShiftAuto */
	u8  cpuClockPhaseShiftAuto;
} DprCmdSetCPUClkConfig;

typedef struct {
	DprCmdHeader header;
	u8  mode;			/* L2EA_MODE_* */
} DprCmdL2EarlyAck;

typedef struct {
	DprCmdHeader header;
	u32 flags;			/* AUDGS_* */
} DprCmdAudioGetState;

/* out-of-range gains are refused, not clamped */
typedef struct {
	DprCmdHeader header;
	u8  channel;			/* AUDCH_* */
	s8  gainDb;
	u8  flags;			/* AUDF_* */
	u8  pad;
} DprCmdAudioSetChannel;

typedef struct {
	s8  gainDb;
	s8  minDb;			/* the lowest audible step; below it only mute */
	s8  maxDb;
	u8  flags;			/* AUDF_* */
} DprAudioChannel;

typedef struct {
	u32 version;			/* AUDIOSTATE_VERSION */
	u32 result;			/* AUDRES_* of the command this replies to */
	u32 channelCount;
	u32 sampleRate;			/* Hz */
	u32 codecFlags;			/* AUDCODEC_F_* */
	u32 codecI2cErrors;
	u32 clipCount;			/* output samples saturated since boot */
	u32 saiErrors;
	u32 reserved[4];
	DprAudioChannel ch[AUDCH_MAX];
} DprAudioState;

typedef struct {
	DprRplHeader header;
	DprAudioState st;
} DprRplAudioState;

#pragma pack(pop)

#endif /* _ASM_M68K_CSWARPAMICOMMDATA_H */
