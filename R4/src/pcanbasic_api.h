#ifndef PCANBASIC_API_H
#define PCANBASIC_API_H

// ---------------------------------------------------------------------------
// Minimal subset of the PEAK-System PCAN-Basic API (Windows PCANBasic.dll).
//
// Only the declarations actually used by CanDLL are provided here, so the
// project builds without the PCAN-Basic SDK. The real PCANBasic.dll is bound
// at run time with QLibrary, so no import library (.lib) is needed at link
// time - the PEAK driver installation provides the DLL.
//
// Values verified against the official PCANBasic.h (PEAK-System Technik GmbH).
// ---------------------------------------------------------------------------

#include <qglobal.h>

// PCAN-Basic API uses the __stdcall calling convention on Windows.
#if defined(Q_OS_WIN)
#  define PCAN_API __stdcall
#else
#  define PCAN_API
#endif

// --- Type definitions ------------------------------------------------------

typedef quint16     TPCANHandle;        // PCAN hardware channel handle (WORD)
typedef quint32     TPCANStatus;        // status / error code (DWORD)
typedef quint8      TPCANParameter;     // parameter to read or set
typedef quint8      TPCANDevice;
typedef quint8      TPCANMessageType;
typedef quint8      TPCANType;
typedef quint8      TPCANMode;
typedef quint16     TPCANBaudrate;      // BTR0/BTR1 register value (WORD)
typedef const char *TPCANBitrateFD;     // CAN FD bit rate string
typedef quint64     TPCANTimestampFD;   // reception time in microseconds

// --- Channel handles -------------------------------------------------------
//
// Only the base handle of each family is listed; CanBus::channelHandle()
// derives every channel numerically (ch 1..8 -> base+ch, ch 9..16 -> high+ch).
// PCAN_ISABUS* and PCAN_PCCBUS* (ISA / PCMCIA cards) keep their historical
// values - the current official PCANBasic.h does not define them anymore.

#define PCAN_NONEBUS              0x00

#define PCAN_ISABUS1              0x21  // .. 0x28  (channels 1..8)
#define PCAN_PCIBUS1              0x41  // .. 0x48  (channels 1..8)
#define PCAN_PCIBUS9              0x409 // .. 0x410 (channels 9..16)
#define PCAN_USBBUS1              0x51  // .. 0x58  (channels 1..8)
#define PCAN_USBBUS9              0x509 // .. 0x510 (channels 9..16)
#define PCAN_PCCBUS1              0x61  // .. 0x62
#define PCAN_LANBUS1              0x801 // .. 0x810 (channels 1..16)

// --- Error / status codes --------------------------------------------------

#define PCAN_ERROR_OK             0x00000
#define PCAN_ERROR_XMTFULL        0x00001
#define PCAN_ERROR_OVERRUN        0x00002
#define PCAN_ERROR_BUSLIGHT       0x00004
#define PCAN_ERROR_BUSHEAVY       0x00008
#define PCAN_ERROR_BUSWARNING     PCAN_ERROR_BUSHEAVY
#define PCAN_ERROR_BUSPASSIVE     0x40000
#define PCAN_ERROR_BUSOFF         0x00010
#define PCAN_ERROR_ANYBUSERR      (PCAN_ERROR_BUSWARNING | PCAN_ERROR_BUSLIGHT | \
                                   PCAN_ERROR_BUSHEAVY | PCAN_ERROR_BUSOFF | \
                                   PCAN_ERROR_BUSPASSIVE)
#define PCAN_ERROR_QRCVEMPTY      0x00020
#define PCAN_ERROR_QOVERRUN       0x00040
#define PCAN_ERROR_QXMTFULL       0x00080
#define PCAN_ERROR_REGTEST        0x00100
#define PCAN_ERROR_NODRIVER       0x00200
#define PCAN_ERROR_HWINUSE        0x00400
#define PCAN_ERROR_NETINUSE       0x00800
#define PCAN_ERROR_ILLHW          0x01400
#define PCAN_ERROR_ILLNET         0x01800
#define PCAN_ERROR_ILLCLIENT      0x01C00
#define PCAN_ERROR_ILLHANDLE      (PCAN_ERROR_ILLHW | PCAN_ERROR_ILLNET | PCAN_ERROR_ILLCLIENT)
#define PCAN_ERROR_RESOURCE       0x02000
#define PCAN_ERROR_ILLPARAMTYPE   0x04000
#define PCAN_ERROR_ILLPARAMVAL    0x08000
#define PCAN_ERROR_UNKNOWN        0x10000
#define PCAN_ERROR_ILLDATA        0x20000
#define PCAN_ERROR_ILLMODE        0x80000
#define PCAN_ERROR_CAUTION        0x02000000
#define PCAN_ERROR_INITIALIZE     0x04000000
#define PCAN_ERROR_ILLOPERATION   0x08000000

// --- Message type flags ------------------------------------------------------

#define PCAN_MESSAGE_STANDARD     0x00
#define PCAN_MESSAGE_RTR          0x01
#define PCAN_MESSAGE_EXTENDED     0x02
#define PCAN_MESSAGE_FD           0x04
#define PCAN_MESSAGE_BRS          0x08
#define PCAN_MESSAGE_ESI          0x10
#define PCAN_MESSAGE_ECHO         0x20
#define PCAN_MESSAGE_ERRFRAME     0x40
#define PCAN_MESSAGE_STATUS       0x80

// --- Receive filter modes (used by CAN_FilterMessages) -----------------------
// Official PCANBasic.h aliases these to the message-type flags:
//   #define PCAN_MODE_STANDARD  PCAN_MESSAGE_STANDARD   (11-bit identifiers)
//   #define PCAN_MODE_EXTENDED  PCAN_MESSAGE_EXTENDED   (29-bit identifiers)

#define PCAN_MODE_STANDARD        PCAN_MESSAGE_STANDARD
#define PCAN_MODE_EXTENDED        PCAN_MESSAGE_EXTENDED

// --- SetValue / GetValue parameters ------------------------------------------

#define PCAN_BUSOFF_AUTORESET     0x07
#define PCAN_LISTEN_ONLY          0x08
#define PCAN_CHANNEL_CONDITION    0x0D

#define PCAN_PARAMETER_OFF        0x00
#define PCAN_PARAMETER_ON         0x01

#define PCAN_CHANNEL_UNAVAILABLE  0x00
#define PCAN_CHANNEL_AVAILABLE    0x01
#define PCAN_CHANNEL_OCCUPIED     0x02
#define PCAN_CHANNEL_PCANVIEW     (PCAN_CHANNEL_AVAILABLE | PCAN_CHANNEL_OCCUPIED)

// --- Classic CAN bit rate codes (BTR0/BTR1) ----------------------------------

#define PCAN_BAUD_1M              0x0014
#define PCAN_BAUD_800K            0x0016
#define PCAN_BAUD_500K            0x001C
#define PCAN_BAUD_250K            0x011C
#define PCAN_BAUD_125K            0x031C
#define PCAN_BAUD_100K            0x432F
#define PCAN_BAUD_95K             0xC34E
#define PCAN_BAUD_83K             0x852B
#define PCAN_BAUD_50K             0x472F
#define PCAN_BAUD_47K             0x1414
#define PCAN_BAUD_33K             0x8B2F
#define PCAN_BAUD_20K             0x532F
#define PCAN_BAUD_10K             0x672F
#define PCAN_BAUD_5K              0x7F7F

// --- Structures (binary compatible with PCANBasic.h) ---------------------------

// Note: no #pragma pack needed - with default alignment every member offset
// and the total struct size match the official PCANBasic.h definitions.

typedef struct tagTPCANMsg
{
    quint32         ID;         // 11/29-bit message identifier
    TPCANMessageType MSGTYPE;   // message type flags
    quint8          LEN;        // data length 0..8
    quint8          DATA[8];
} TPCANMsg;

typedef struct tagTPCANTimestamp
{
    quint32         millis;             // base value: milliseconds
    quint16         millis_overflow;    // roll-arounds of millis
    quint16         micros;             // 0..999
} TPCANTimestamp;

typedef struct tagTPCANMsgFD
{
    quint32         ID;         // 11/29-bit message identifier
    TPCANMessageType MSGTYPE;   // message type flags
    quint8          DLC;        // data length code 0..15 (0..64 bytes)
    quint8          DATA[64];
} TPCANMsgFD;

// --- Function pointer types (resolved from PCANBasic.dll) ----------------------

typedef TPCANStatus (PCAN_API *FnCAN_Initialize)(TPCANHandle Channel, TPCANBaudrate Btr0Btr1,
                                                 TPCANType HwType, quint32 IOPort, quint16 Interrupt);
typedef TPCANStatus (PCAN_API *FnCAN_InitializeFD)(TPCANHandle Channel, TPCANBitrateFD BitrateFD);
typedef TPCANStatus (PCAN_API *FnCAN_Uninitialize)(TPCANHandle Channel);
typedef TPCANStatus (PCAN_API *FnCAN_Reset)(TPCANHandle Channel);
typedef TPCANStatus (PCAN_API *FnCAN_GetStatus)(TPCANHandle Channel);
typedef TPCANStatus (PCAN_API *FnCAN_Read)(TPCANHandle Channel, TPCANMsg *MessageBuffer,
                                           TPCANTimestamp *TimestampBuffer);
typedef TPCANStatus (PCAN_API *FnCAN_ReadFD)(TPCANHandle Channel, TPCANMsgFD *MessageBuffer,
                                             TPCANTimestampFD *TimestampBuffer);
typedef TPCANStatus (PCAN_API *FnCAN_Write)(TPCANHandle Channel, const TPCANMsg *MessageBuffer);
typedef TPCANStatus (PCAN_API *FnCAN_WriteFD)(TPCANHandle Channel, const TPCANMsgFD *MessageBuffer);
typedef TPCANStatus (PCAN_API *FnCAN_FilterMessages)(TPCANHandle Channel, quint32 FromID,
                                                     quint32 ToID, TPCANMode Mode);
typedef TPCANStatus (PCAN_API *FnCAN_GetValue)(TPCANHandle Channel, TPCANParameter Parameter,
                                               void *Buffer, quint32 BufferLength);
typedef TPCANStatus (PCAN_API *FnCAN_SetValue)(TPCANHandle Channel, TPCANParameter Parameter,
                                               void *Buffer, quint32 BufferLength);
typedef TPCANStatus (PCAN_API *FnCAN_GetErrorText)(TPCANStatus Error, quint16 Language, char *Buffer);

#endif // PCANBASIC_API_H
