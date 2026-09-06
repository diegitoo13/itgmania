#ifndef ASIO_INTERFACE_H
#define ASIO_INTERFACE_H

/*
 * Host-side declarations for the Steinberg ASIO 2.x driver interface.
 *
 * This file deliberately declares the ASIO interface itself instead of
 * including the Steinberg ASIO SDK, for two reasons:
 *
 * 1. Licensing. The ASIO SDK is dual-licensed (proprietary / GPLv3). The
 *    GPLv3 option is not compatible with ITGmania's MIT-style license for
 *    distributed binaries, and the proprietary option requires a signed
 *    agreement with Steinberg. This file contains no Steinberg code; it
 *    declares the binary interface (COM vtable layout and data structures)
 *    that every ASIO driver has implemented unchanged since ASIO 2.0.
 *
 * 2. Build simplicity. Every build of ITGmania can use any installed ASIO
 *    driver without downloading an SDK first.
 *
 * The declarations below follow the ASIO 2.3 specification. Only the
 * host-side surface is declared (IASIO plus the structures it uses); driver
 * development interfaces (IASIOHost etc.) are intentionally omitted.
 *
 * ASIO drivers are COM in-process servers. A host instantiates a driver by
 * CLSID (found under HKLM\SOFTWARE\ASIO\<driver name>) and talks to the
 * IASIO interface below.
 */

// clang-format off
#include <windows.h>
// clang-format on

typedef long ASIOError;
typedef long ASIOBool;

enum {
  ASE_OK = 0,
  ASE_SUCCESS = 0x3f4847a0,
  ASE_NotPresent = -1000,  // hardware input or output is not present
  ASE_HWMalfunction,       // hardware is malfunctioning
  ASE_InvalidParameter,    // input parameter invalid
  ASE_InvalidMode,         // hardware is in a bad mode or used in a bad mode
  ASE_SPNotAdvancing,      // hardware is not running when sample position
                           // is asked
  ASE_NoClock,             // sample clock or rate cannot be determined or is
                           // not present
  ASE_NoMemory             // not enough memory for completing the request
};

enum {
  ASIOFalse = 0,
  ASIOTrue = 1
};

// Sample data types. "MSB" formats are big-endian, "LSB" little-endian.
// The Int32*16/18/20/24 types are 32-bit containers with the significant
// bits left-justified (e.g. ASIOSTInt32LSB24 carries 24 significant bits).
enum ASIOSampleType {
  ASIOSTInt16MSB = 0,
  ASIOSTInt24MSB = 1,  // used for 20 bits as well
  ASIOSTInt32MSB = 2,
  ASIOSTFloat32MSB = 3,  // IEEE 754 32 bit float
  ASIOSTFloat64MSB = 4,  // IEEE 754 64 bit double float

  // 32 bit data buffer, with different alignment of the data inside
  ASIOSTInt32MSB16 = 8,   // 32 bit data with 16 bit alignment
  ASIOSTInt32MSB18 = 9,   // 32 bit data with 18 bit alignment
  ASIOSTInt32MSB20 = 10,  // 32 bit data with 20 bit alignment
  ASIOSTInt32MSB24 = 11,  // 32 bit data with 24 bit alignment

  ASIOSTInt16LSB = 16,
  ASIOSTInt24LSB = 17,
  ASIOSTInt32LSB = 18,
  ASIOSTFloat32LSB = 19,  // IEEE 754 32 bit float, as found on Intel x86
  ASIOSTFloat64LSB = 20,  // IEEE 754 64 bit double float, as found on Intel

  ASIOSTInt32LSB16 = 24,  // 32 bit data with 16 bit alignment
  ASIOSTInt32LSB18 = 25,  // 32 bit data with 18 bit alignment
  ASIOSTInt32LSB20 = 26,  // 32 bit data with 20 bit alignment
  ASIOSTInt32LSB24 = 27,  // 32 bit data with 24 bit alignment

  // DSD (1 bit) formats; not supported by this host.
  ASIOSTDSDInt8LSB1 = 32,
  ASIOSTDSDInt8MSB1 = 33,
  ASIOSTDSDInt8NER8 = 40
};

typedef struct {
  unsigned long hi;
  unsigned long lo;
} ASIOSamples, ASIOTimeStamp;

typedef struct {
  long isInput;        // on input:  ASIOTrue: input, else output
  long channelNum;     // on input:  channel index
  void* buffers[2];    // on output: double-buffer addresses (filled in by
                       // the driver)
} ASIOBufferInfo;

typedef struct {
  long channel;            // on input: channel index
  long isInput;            // on input:  ASIOTrue: input, else output
  char name[32];           // on output: null-terminated channel name
  long type;               // on output: ASIOSampleType
  char future[32];
} ASIOChannelInfo;

typedef struct {
  long index;             // as used for setClockSource()
  long associatedChannel; // channel associated (e.g. ADAT in a specific card)
  long associatedGroup;
  long isCurrentSource;   // ASIOTrue if this is the current clock source
  char name[32];          // for user selection
} ASIOClockSource;

// Time information passed to bufferSwitchTimeInfo. Not used by this host
// (positions are queried via getSamplePosition), declared for completeness.
typedef struct {
  double speed;                // absolute speed (1. = nominal)
  ASIOTimeStamp systemTime;    // system time related to samplePosition
  ASIOSamples samplePosition;
  double sampleRate;           // current rate
  unsigned long flags;         // validity flags
  char reserved[12];
} ASIOTimeInfo;

typedef struct {
  double speed;
  ASIOSamples timeCodeSamples; // time in samples
  unsigned long flags;         // some information flags
  char future[64];
} ASIOTimeCode;

typedef struct {
  long reserved[4];            // must be 0
  ASIOTimeInfo timeInfo;       // required
  ASIOTimeCode timeCode;       // optional, evaluated if "timeCode" flag set
} ASIOTime;

typedef struct {
  // bufferSwitch indicates that both input and output are to be processed.
  // directProcess: if 0, the driver expects the host to perform output
  // processing after bufferSwitch; if nonzero, processing must happen inside
  // the callback.
  void (*bufferSwitch)(long doubleBufferIndex, long directProcess);
  // Called when the driver detects a sample rate change on the hardware.
  void (*sampleRateDidChange)(double sRate);
  // Generic driver-to-host message; see the selectors below.
  long (*asioMessage)(long selector, long value, void* message, double* opt);
  // Extended buffer switch with timing information. May be used instead of
  // bufferSwitch by drivers supporting time info.
  void* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex,
                                long directProcess);
} ASIOCallbacks;

// asioMessage selectors.
enum {
  kAsioSelectorSupported = 1,  // selector in value, returns 1 if supported
  kAsioEngineVersion,          // returns engine (host) version
  kAsioResetRequest,           // request driver reset; reinitialize at a
                               // safe point
  kAsioBufferSizeChange,       // new buffer size took effect; re-create
                               // buffers at a safe point
  kAsioResyncRequest,          // the driver detected a loss of sync
  kAsioLatenciesChanged,       // latencies changed; re-query getLatencies
  kAsioSupportsTimeInfo,       // driver asks if host uses time info
  kAsioSupportsTimeCode,       // driver asks if host uses time code
  kAsioMMCCommand,             // MMC command from the driver
  kAsioSupportsInputMonitor,
  kAsioSupportsInputGain,
  kAsioSupportsInputMeter,
  kAsioSupportsOutputGain,
  kAsioSupportsOutputMeter,
  kAsioOverload,               // driver detected an overload
  kAsioNumMessageSelectors
};

struct IASIOVtbl;
struct IASIO {
  IASIOVtbl* vtbl;
};

// Layout-compatible with the IASIO COM interface (including IUnknown).
// ASIO 2.x is ABI-frozen: drivers written against any SDK version implement
// exactly this vtable.
struct IASIOVtbl {
  // IUnknown
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(IASIO*, const IID*, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(IASIO*);
  ULONG(STDMETHODCALLTYPE* Release)(IASIO*);

  // IASIO
  ASIOBool(*init)(IASIO*, void* sysHandle);
  void (*getDriverName)(IASIO*, char* name);  // max 32 bytes incl. terminator
  long (*getDriverVersion)(IASIO*);
  void (*getErrorMessage)(IASIO*, char* string);  // max 124 bytes
  ASIOError(*start)(IASIO*);
  ASIOError(*stop)(IASIO*);
  ASIOError(*getChannels)(IASIO*, long* numInputChannels,
                          long* numOutputChannels);
  ASIOError(*getLatencies)(IASIO*, long* inputLatency, long* outputLatency);
  ASIOError(*getBufferSize)(IASIO*, long* minSize, long* maxSize,
                            long* preferredSize, long* granularity);
  ASIOError(*canSampleRate)(IASIO*, double sampleRate);
  ASIOError(*getSampleRate)(IASIO*, double* sampleRate);
  ASIOError(*setSampleRate)(IASIO*, double sampleRate);
  ASIOError(*getClockSources)(IASIO*, ASIOClockSource* clocks, long* numSources);
  ASIOError(*setClockSource)(IASIO*, long reference);
  ASIOError(*getSamplePosition)(IASIO*, ASIOSamples* sPos,
                                ASIOTimeStamp* tStamp);
  ASIOError(*getChannelInfo)(IASIO*, ASIOChannelInfo* info);
  ASIOError(*createBuffers)(IASIO*, ASIOBufferInfo* bufferInfos,
                            long numChannels, long bufferSize,
                            ASIOCallbacks* callbacks);
  ASIOError(*disposeBuffers)(IASIO*);
  ASIOError(*controlPanel)(IASIO*);
  ASIOError(*future)(IASIO*, long selector, void* opt);
  ASIOError(*outputReady)(IASIO*);
};

#endif  // ASIO_INTERFACE_H
