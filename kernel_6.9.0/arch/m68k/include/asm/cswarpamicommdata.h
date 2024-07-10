#ifndef CSWARPAMICOMMDATA_H
#define CSWARPAMICOMMDATA_H

#include "cswarpamicommdata_sd.h"

#define AMICOMM_DBGMSG_LEN        256
#define AMICOMM_PATH_LEN          128
#define AMICOMM_BOARDNAME_LEN     32
#define AMICOMM_USB_HOSTS         2
#define AMICOMM_WIFI_SSID_LEN     128
#define AMICOMM_WIFI_PASS_LEN     128

// -------------------------------------------------------------
// FPGA DualPort RAM communication
// Interrupts control register
// -------------------------------------------------------------
#define DPREG_CR_SET        0x80000000  // set bits in register
#define DPREG_CR_CLR        0x00000000  // clr bits in register
#define DPREG_CR_IE_ARM     (1UL << 0)  // ARM irq enable
#define DPREG_CR_IE_68K     (1UL << 1)  // 68K irq enable
#define DPREG_CR_MP_ARM     (1UL << 2)  // Msg pending (ARM)
#define DPREG_CR_MP_68K     (1UL << 3)  // Msg pending (68K)
#define DPREG_CR_MR_ARM     (1UL << 4)  // Msg received (ARM)
#define DPREG_CR_MR_68K     (1UL << 5)  // Msg received (68K)
// ethernet IRQ's (68k)
#define DPREG_CR_IE_ETHRX   (1UL << 6)  // ETH rx irq enable
#define DPREG_CR_IE_ETHTX   (1UL << 7)  // ETH tx irq enable
#define DPREG_CR_IE_ETHST   (1UL << 8)  // ETH state irq enable
#define DPREG_CR_IF_ETHRX   (1UL << 9)  // ETH rx irq
#define DPREG_CR_IF_ETHTX   (1UL << 10) // ETH tx irq
#define DPREG_CR_IF_ETHST   (1UL << 11) // ETH state irq

// Volume masks
#define AUDVOLMASK_MIX_AMIGA  0x01
#define AUDVOLMASK_MIX_MP3    0x02
#define AUDVOLMASK_MASTER     0x04

// Disk IO
#define DISK_MAX_DPRAM_TRANSFER	7
#define DISK_BLOCKSIZE 512
#define DISK_NR_SD 0
#define DISK_NR_USB 1

// ETH/WIFI
#define ETH_MTU_AND_HDR_SIZE    (1500 + 14)
#define ETH_MAC_SIZE  6

#pragma pack(2)

typedef enum {
  wacOK = 0,
  wacTIMEOUT,
  wacBREAK,
  wacNOTINITIALIZED,
  wacCOMERR,
  wacBUFFERR,
} WarpAmiCommStatus;

// -------------------------------------------------------------
// DualPort RAM communication
// Command (sent FROM MC68060) Data types
// -------------------------------------------------------------
// main command types (send from Amiga)
typedef enum {
  dpcmdNop,
  dpcmdDbgMsg,  
  dpcmdJpegTest,
  dpcmdAudioTest,
  dpcmdSetCpuTurbo,
  dpcmdSelectKick,
  dpcmdGetDiag,
  dpcmdSetIdeMode,
  dpcmdSetIdeSpeed,
  dpcmdSetHIDMouseRes,
  dpcmdSetWiFiSSID,
  dpcmdSetWiFiPass,
  dpcmdSetTempRegulator,
  dpcmdSetTimeZoneShift,
  dpcmdOpenDir,
  dpcmdCloseDir,
  dpcmdReadDir,
  dpcmdSDGetInfo,
  dpcmdDiskWriteBlocks,
  dpcmdDiskReadBlocks,
  dpcmdGetHIDMouseRes,
  dpcmdUSBDiskGetInfo,
  dpcmdGetARMInfo,
  dpcmdEthTransmit,
  dpcmdEthReceive,
  dpcmdEthGetMACAddr,
  dpcmdGetMouseWheelData,
} DprCmd;

// Audio command types
typedef enum {
  audcmdStop = 0,
  audcmdPlay,
  audcmdPause,
  audcmdSetVolumes,
} AudioCmd;

  // WiFi states
typedef enum {
  wifiUnknown,
  wifiStarted,
  wifiStopped,
  wifiConnected,
  wifiDisconnected
} WiFiState;

// common command header
typedef struct {
  uint32_t  cmd;
} DprCmdHeader;

// debug message sent from Amiga
typedef struct {
  DprCmdHeader  header;
  uint8_t       msgType;
  char          msg[AMICOMM_DBGMSG_LEN];
} DprCmdDbgMsg;

// JPEG decomp request
typedef struct {
  DprCmdHeader  header;
  uint8_t       bitsPerPixel;
  char          fileName[AMICOMM_PATH_LEN];          
} DprCmdJpegTest;

// AUDIO command data
typedef struct {
  DprCmdHeader  header;
  uint8_t       audioCmd;
  uint8_t       volSetMask;
  uint8_t       mixAmiga;
  uint8_t       mixMp3;
  uint8_t       masterVolume;
  char          fileName[AMICOMM_PATH_LEN];
} DprCmdAudioTest;

// Set M68k CPU turbo level command data
typedef struct {
  DprCmdHeader  header;
  uint32_t      turboLevel;
} DprCmdSetCpuTurbo;

// Kickstart nr selection command data
typedef struct {
  DprCmdHeader header;
  uint8_t kickNr;
} DprCmdSelectKick;

// IDE mode selection command data
typedef struct {
  DprCmdHeader header;
  uint8_t nativeIdeEnable;
} DprCmdIdeMode;

// IDE speed command data
typedef struct {
  DprCmdHeader header;
  uint8_t ataIOR_as;
  uint8_t ataIOR_ng;
  uint8_t ataIOW_as;
  uint8_t ataIOW_ng;
  uint8_t ataACK_as;
} DprCmdIdeSpeed;

// HID mouse resolution command data
typedef struct {
  DprCmdHeader header;
  uint16_t hidMouseRes; // fixed point multiplier 256=1.0
} DprCmdHIDMouseRes;

// WiFi SSID setting
typedef struct {
  DprCmdHeader header;
  char ssid[AMICOMM_WIFI_SSID_LEN];
} DprCmdWiFiSSID;

// WiFi Password setting
typedef struct {
  DprCmdHeader header;
  char pass[AMICOMM_WIFI_PASS_LEN];
} DprCmdWiFiPass;

// Time zone shift
typedef struct {
  DprCmdHeader header;
  int32_t shiftSecs;
} DprCmdTimeZoneShift;

// Temperature regulator settings
typedef struct {
  DprCmdHeader header;
  int32_t mc68kTemp;
  int32_t minPwmPercent;
} DprCmdTempReg;

// Open dir command
typedef struct {
  DprCmdHeader header;
  char path[AMICOMM_PATH_LEN];
} DprCmdOpenDir;

// Disk ReadBlocks
typedef struct {
  DprCmdHeader header;
  uint32_t blockAddr;
  uint32_t readBlocksCnt;
  uint32_t dmaDdrAddr;
  uint8_t dmaEnable;
  uint8_t diskNr;
} DprCmdDiskReadBlocks;

// Disk WriteBlocks
typedef struct {
	DprCmdHeader header;
	uint32_t blockAddr;
	uint32_t writeBlocksCnt;
	uint8_t data[DISK_MAX_DPRAM_TRANSFER * DISK_BLOCKSIZE];
  uint32_t dmaDdrAddr;
  uint8_t dmaEnable;
  uint8_t diskNr;
} DprCmdDiskWriteBlocks;

// Eth send packet
typedef struct {
  DprCmdHeader header;
  uint16_t pktSize;
  uint8_t packet[ETH_MTU_AND_HDR_SIZE];
} DprCmdEthSend;

// Command communication frame
typedef union {
  DprCmdHeader  header;
  DprCmdDbgMsg  dbgMsg;
  DprCmdJpegTest jpegTest;
  DprCmdAudioTest audioTest;
  DprCmdSetCpuTurbo setCpuTurbo;
  DprCmdSelectKick kickSel;
  DprCmdIdeMode ideMode;
  DprCmdIdeSpeed ideSpeed;
  DprCmdHIDMouseRes hidMouseRes;
  DprCmdWiFiSSID wifiSSID;
  DprCmdWiFiPass wifiPass;
  DprCmdTempReg tempReg;
  DprCmdTimeZoneShift timeZone;
  DprCmdOpenDir openDir;
  DprCmdDiskReadBlocks diskRead;
  DprCmdDiskWriteBlocks diskWrite;
  DprCmdEthSend ethSend;
} DprCmdFrame;

// -------------------------------------------------------------
// DualPort RAM communication
// Replies (sent TO MC68060) Data types
// -------------------------------------------------------------
// main reply types (send from ARM)
typedef enum {
  dprplNop,
  dprplDiagFrame,
  dprplOpenDirStatus,
  dprplReadDir,
  dprplSDGetInfo,
  dprplDiskReadBlocks,
  dprplDiskWriteBlocks,
  dprplGetHIDMouseRes,
  dprplUSBGetInfo,
  dprplARMInfo,
  dprplEthReceive,
  dprplEthMACAddr,
  dprplMouseWheelData,
} DprRpl;

// common reply header
typedef struct {
  uint32_t  rpl;
} DprRplHeader;

typedef enum {USBDevNone = 0, USBDevHID, USBDevMassStorage, USBDevOther} USBDevStatus;
const char *USBDevStatusStr[] = {"No Device", "HID Device", "Mass Storage", "Other Device"};

// Diagnostic data frame
typedef struct {
  DprRplHeader  header;
  float         vcc5v;
  float         vBatt;
  float         t60ntc;
  float         t60internal;
  float         tArm;
  uint32_t      hwVer;
  uint32_t      fmwVer;
  uint32_t      currentTurboLevel;
  uint32_t      fanPercent;
  char          boardName[AMICOMM_BOARDNAME_LEN];
  uint32_t      kickstartNr;
  uint32_t      usbDevStatus[AMICOMM_USB_HOSTS];
  char          wifiSSID[AMICOMM_WIFI_SSID_LEN];
  char          wifiPass[AMICOMM_WIFI_PASS_LEN];
  uint32_t      nativeIdeEnabled;
  int32_t       tempRegCpu;
  int32_t       tempRegMinPwm;
  uint8_t       IDE_IOR_as;
  uint8_t       IDE_IOR_ng;
  uint8_t       IDE_IOW_as;
  uint8_t       IDE_IOW_ng;
  uint8_t       IDE_ACK_as;
  int32_t       timeZoneCorrSecs;
  uint8_t       warpBoardType;
  uint32_t      wifiState;
} DprRplDiagMsg;

// Open dir status
typedef struct {
  DprRplHeader header;
  uint32_t success;
} DprRplOpenDirStatus;

// Read dir data
typedef struct {
  DprRplHeader header;
  char name[AMICOMM_PATH_LEN];
  uint32_t size;
  uint16_t date;
  uint16_t time;
  uint8_t isDir;
  uint8_t isSys;
  uint8_t isReadOnly;
  uint8_t isHidden;
} DprRplDirEntry;

// SD Card GetInfo reply
typedef struct {
	DprRplHeader header;
	uint8_t cardInitialized;
	uint32_t state;
	uint32_t type;
	uint32_t blockNbr;
	uint32_t blockSize;
	AmiCommSD_CSD csd;
	AmiCommSD_CID cid;
} DprRplSDGetInfo;

// Disk ReadBlocks reply
typedef struct {
	DprRplHeader header;
	uint32_t readBlocksCnt;
	uint8_t data[DISK_MAX_DPRAM_TRANSFER * DISK_BLOCKSIZE];
} DprRplDiskReadBlocks;

// Disk WriteBlocks reply
typedef struct {
	DprRplHeader header;
	uint32_t writeBlocksCnt;
} DprRplDiskWriteBlocks;

// get HID mouse resolution reply data
typedef struct {
  DprRplHeader header;
  uint16_t hidMouseRes; // fixed point multiplier 256=1.0
} DprRplHIDMouseRes;

// USB Disk GetInfo reply
typedef struct {
	DprRplHeader header;
	uint8_t diskInitialized;
	uint32_t state;
	uint32_t type;
	uint32_t blockNbr;
	uint32_t blockSize;
} DprRplUSBGetInfo;

// ARM info data
typedef struct {
  DprRplHeader header;
  uint32_t cpuRevId;
  uint32_t halVersion;
} DprRplARMInfo;

// Eth receive packet
typedef struct {
  DprRplHeader header;
  uint16_t pktSize;
  uint8_t packet[ETH_MTU_AND_HDR_SIZE];
} DprRplEthRecv;

typedef struct {
  DprRplHeader header;
  uint8_t mac[ETH_MAC_SIZE];
} DprRplEthMACAddr;

typedef struct {
  DprRplHeader header;
  int8_t mouseWheelCnt;
} DprRplMouseWheelData;

// Reply communication frame
typedef union {
  DprRplHeader  header;
  DprRplDiagMsg diag;
  DprRplOpenDirStatus openDirStatus;
  DprRplDirEntry dirEntry;
  DprRplSDGetInfo sdInfo;
  DprRplDiskReadBlocks diskRead;
  DprRplDiskWriteBlocks diskWrite;
  DprRplHIDMouseRes hidMouseRes;
  DprRplUSBGetInfo usbInfo;
  DprRplARMInfo armInfo;
  DprRplEthRecv ethRecv;
  DprRplEthMACAddr ethMAC;
  DprRplMouseWheelData mouseWheel;
} DprRplFrame;

#pragma pack()

#endif // CSWARPAMICOMMDATA_H

