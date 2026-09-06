#include "RageSoundDriver_ASIO.h"

// clang-format off
#include <windows.h>
#include <ole2.h>
// clang-format on

#include <algorithm>
#include <cmath>
#include <vector>

#include "ASIOInterface.h"
#include "Preference.h"
#include "PrefsManager.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "StdString.h"
#include "archutils/Win32/GraphicsWindow.h"
#include "global.h"

REGISTER_SOUND_DRIVER_CLASS2("ASIO", ASIO);

/* Which installed ASIO driver to use. Matched against the registry names
 * under HKLM\SOFTWARE\ASIO (exact match first, then substring). Empty means
 * automatic: the only driver is used, or the first one with a log message
 * explaining how to choose. Set to the actual driver name on successful
 * initialization so the choice survives hardware changes. */
static Preference<std::string> g_sASIODriverName("ASIODriver", "");

namespace {

struct ASIODriverEntry {
  std::string m_sName;
  std::string m_sClsid;
  std::string m_sDescription;
};

const char* AsioErrorString(ASIOError iError) {
  switch (iError) {
    case ASE_OK: return "OK";
    case ASE_SUCCESS: return "success";
    case ASE_NotPresent: return "hardware not present";
    case ASE_HWMalfunction: return "hardware malfunction";
    case ASE_InvalidParameter: return "invalid parameter";
    case ASE_InvalidMode: return "invalid mode";
    case ASE_SPNotAdvancing: return "sample position not advancing";
    case ASE_NoClock: return "no sample clock";
    case ASE_NoMemory: return "out of memory";
    default: return "unknown";
  }
}

bool AsioSuccess(ASIOError iError) {
  return iError == ASE_OK || iError == ASE_SUCCESS;
}

/* Enumerate the 64-bit ASIO drivers registered under HKLM\SOFTWARE\ASIO.
 * Each subkey is a driver name and carries a CLSID value pointing at the
 * driver's COM class. */
std::vector<ASIODriverEntry> EnumerateASIODrivers() {
  std::vector<ASIODriverEntry> vDrivers;

  HKEY hRoot = nullptr;
  if (RegOpenKeyExA(
          HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &hRoot) !=
      ERROR_SUCCESS) {
    return vDrivers;
  }

  char szSubkey[256];
  for (DWORD iIndex = 0;
       RegEnumKeyA(hRoot, iIndex, szSubkey, sizeof(szSubkey)) ==
       ERROR_SUCCESS;
       ++iIndex) {
    HKEY hSub = nullptr;
    if (RegOpenKeyExA(hRoot, szSubkey, 0, KEY_READ, &hSub) != ERROR_SUCCESS) {
      continue;
    }

    ASIODriverEntry entry;
    entry.m_sName = szSubkey;

    // Read the CLSID; skip subkeys that don't carry one.
    DWORD iType = 0, iSize = 0;
    if (RegQueryValueExA(hSub, "CLSID", nullptr, &iType, nullptr, &iSize) !=
            ERROR_SUCCESS ||
        iSize == 0) {
      RegCloseKey(hSub);
      continue;
    }
    std::vector<char> sClsid(iSize, 0);
    if (RegQueryValueExA(
            hSub, "CLSID", nullptr, nullptr, (LPBYTE)sClsid.data(), &iSize) !=
        ERROR_SUCCESS) {
      RegCloseKey(hSub);
      continue;
    }
    entry.m_sClsid = sClsid.data();

    iSize = 0;
    if (RegQueryValueExA(hSub, "Description", nullptr, &iType, nullptr,
                         &iSize) == ERROR_SUCCESS &&
        iSize > 0) {
      std::vector<char> sDesc(iSize, 0);
      if (RegQueryValueExA(
              hSub, "Description", nullptr, nullptr, (LPBYTE)sDesc.data(),
              &iSize) == ERROR_SUCCESS) {
        entry.m_sDescription = sDesc.data();
      }
    }

    vDrivers.push_back(entry);
    RegCloseKey(hSub);
  }
  RegCloseKey(hRoot);
  return vDrivers;
}

typedef HRESULT(STDMETHODCALLTYPE* DllGetClassObjectFn)(REFCLSID, REFIID,
                                                        LPVOID*);

/* Instantiate the driver through COM. If the COM catalog registration is
 * broken (CoCreateInstance cannot find the class), fall back to reading
 * InprocServer32 directly and loading the module ourselves; this is still
 * fully generic and covers drivers with damaged registrations. */
IASIO* CreateASIOInstance(
    const CLSID& clsid, const std::string& sClsid, void** pModuleOut) {
  *pModuleOut = nullptr;

  IASIO* pASIO = nullptr;
  HRESULT hr = CoCreateInstance(
      clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**)&pASIO);
  if (SUCCEEDED(hr)) {
    return pASIO;
  }
  LOG->Warn(
      "ASIO: CoCreateInstance(%s) failed (0x%08lx), trying direct module "
      "load",
      sClsid.c_str(), (unsigned long)hr);

  char szPath[1024];
  DWORD iSize = sizeof(szPath);
  std::string sKey = "CLSID\\" + sClsid + "\\InprocServer32";
  if (RegQueryValueA(
          HKEY_CLASSES_ROOT, sKey.c_str(), szPath, (LONG*)&iSize) !=
      ERROR_SUCCESS) {
    return nullptr;
  }

  wchar_t wPath[1024];
  MultiByteToWideChar(CP_ACP, 0, szPath, -1, wPath, ARRAYSIZE(wPath));
  HMODULE hModule = LoadLibraryW(wPath);
  if (hModule == nullptr) {
    LOG->Warn("ASIO: LoadLibraryW(\"%s\") failed", szPath);
    return nullptr;
  }

  DllGetClassObjectFn pGetClassObject =
      (DllGetClassObjectFn)GetProcAddress(hModule, "DllGetClassObject");
  IClassFactory* pFactory = nullptr;
  if (pGetClassObject != nullptr) {
    hr = pGetClassObject(clsid, IID_IClassFactory, (void**)&pFactory);
  }
  if (pFactory != nullptr) {
    hr = pFactory->CreateInstance(nullptr, clsid, (void**)&pASIO);
    pFactory->Release();
  }
  if (pASIO == nullptr) {
    FreeLibrary(hModule);
    return nullptr;
  }
  *pModuleOut = (void*)hModule;
  return pASIO;
}

bool MatchASIODriver(
    const std::vector<ASIODriverEntry>& vDrivers, const std::string& sWanted,
    size_t& iIndexOut) {
  // Exact match first, then substring, both case-insensitive.
  for (int iPass = 0; iPass < 2; ++iPass) {
    for (size_t i = 0; i < vDrivers.size(); ++i) {
      const std::string& sName = vDrivers[i].m_sName;
      if (iPass == 0 ? !EqualsNoCase(sName, sWanted)
                     : sName.find(sWanted) == std::string::npos) {
        continue;
      }
      iIndexOut = i;
      return true;
    }
  }
  return false;
}

/* Pick which driver to use and remember a successful choice. See the
 * comment on g_sASIODriverName for the selection rules. */
bool SelectASIODriver(
    CLSID& clsidOut, std::string& sNameOut, std::string& sError) {
  std::vector<ASIODriverEntry> vDrivers = EnumerateASIODrivers();
  if (vDrivers.empty()) {
    sError = "No ASIO drivers registered (HKLM\\SOFTWARE\\ASIO is empty)";
    return false;
  }

  size_t iPick = vDrivers.size();
  const std::string sWanted = g_sASIODriverName.Get();
  if (!sWanted.empty()) {
    if (MatchASIODriver(vDrivers, sWanted, iPick)) {
      LOG->Info(
          "ASIO: using driver '%s' (selected by ASIODriver preference)",
          vDrivers[iPick].m_sName.c_str());
    } else {
      LOG->Warn(
          "ASIO: ASIODriver preference names '%s', which is not installed; "
          "falling back to automatic selection",
          sWanted.c_str());
    }
  }

  if (iPick == vDrivers.size()) {
    iPick = 0;
    if (vDrivers.size() > 1) {
      std::string sList;
      for (const ASIODriverEntry& driver : vDrivers) {
        if (!sList.empty()) {
          sList += ", ";
        }
        sList += "\"" + driver.m_sName + "\"";
      }
      LOG->Info(
          "ASIO: %u drivers installed (%s); using \"%s\". Set the "
          "ASIODriver preference to another name to override.",
          (unsigned)vDrivers.size(), sList.c_str(),
          vDrivers[iPick].m_sName.c_str());
    }
  }

  sNameOut = vDrivers[iPick].m_sName;

  wchar_t wClsid[256];
  MultiByteToWideChar(
      CP_ACP, 0, vDrivers[iPick].m_sClsid.c_str(), -1, wClsid,
      ARRAYSIZE(wClsid));
  if (CLSIDFromString(wClsid, &clsidOut) != S_OK) {
    sError = ssprintf(
        "Bad CLSID '%s' in ASIO registration for '%s'",
        vDrivers[iPick].m_sClsid.c_str(), sNameOut.c_str());
    return false;
  }
  return true;
}

/* Drivers that don't implement getChannelInfo commonly return ASE_OK with
 * the struct untouched, which reads as type 0 (Int16MSB) with an empty
 * name. A big-endian 16-bit report from a Windows driver with no channel
 * name is not plausible, so treat it as "unimplemented" and assume the
 * overwhelmingly common 32-bit little-endian format. */
long SanitizeSampleType(long iReportedType, const char* szChannelName) {
  if (iReportedType == ASIOSTInt16MSB && szChannelName[0] == '\0') {
    LOG->Warn(
        "ASIO: getChannelInfo returned an empty record; assuming "
        "Int32LSB output format");
    return ASIOSTInt32LSB;
  }
  return iReportedType;
}

// Single live instance for the C callbacks. Only one sound driver exists at
// a time.
RageSoundDriver_ASIO* g_pASIODriver = nullptr;

void CALLBACK Shim_BufferSwitch(long iIndex, long iDirectProcess) {
  if (g_pASIODriver != nullptr) {
    g_pASIODriver->BufferSwitch(iIndex);
  }
}

void CALLBACK Shim_SampleRateDidChange(double dRate) {
  if (g_pASIODriver != nullptr) {
    g_pASIODriver->HandleSampleRateChange(dRate);
  }
}

long CALLBACK Shim_AsioMessage(
    long iSelector, long iValue, void* pMessage, double* pdOpt) {
  if (g_pASIODriver != nullptr) {
    return g_pASIODriver->HandleAsioMessage(iSelector, iValue);
  }
  return 0;
}

void* CALLBACK Shim_BufferSwitchTimeInfo(
    ASIOTime* pParams, long iIndex, long iDirectProcess) {
  if (g_pASIODriver != nullptr) {
    g_pASIODriver->BufferSwitch(iIndex);
  }
  return nullptr;
}

ASIOCallbacks g_ASIOCallbacks = {
    Shim_BufferSwitch, Shim_SampleRateDidChange, Shim_AsioMessage,
    Shim_BufferSwitchTimeInfo};

}  // namespace

RageSoundDriver_ASIO::RageSoundDriver_ASIO()
    : m_pASIO(nullptr),
      m_hASIOModule(nullptr),
      m_iSampleRate(0),
      m_lBufferSize(0),
      m_lOutputLatency(0),
      m_lNumOutputs(0),
      m_iHardwareFrame(0),
      m_bReconfigureRequested(false),
      m_bLatenciesChanged(false),
      m_iOverloadCount(0),
      m_dReportedSampleRate(-1.0),
      m_bRunning(false) {
  m_lSampleType[0] = m_lSampleType[1] = ASIOSTInt32LSB;
  m_pOutputBuffers[0][0] = m_pOutputBuffers[0][1] = nullptr;
  m_pOutputBuffers[1][0] = m_pOutputBuffers[1][1] = nullptr;
}

RageSoundDriver_ASIO::~RageSoundDriver_ASIO() { ShutdownASIO(); }

void RageSoundDriver_ASIO::ShutdownASIO() {
  if (m_pASIO != nullptr && m_bRunning) {
    m_bRunning = false;
    m_pASIO->vtbl->stop(m_pASIO);
  }
  /* The base class destructor shuts down the decode thread; it only fills
   * mix buffers and never touches the ASIO driver, so it is safe to stop
   * the hardware first. */
  if (m_pASIO != nullptr) {
    m_pASIO->vtbl->disposeBuffers(m_pASIO);
    g_pASIODriver = nullptr;
    m_pASIO->vtbl->Release(m_pASIO);
    m_pASIO = nullptr;
  }
  if (m_hASIOModule != nullptr) {
    FreeLibrary((HMODULE)m_hASIOModule);
    m_hASIOModule = nullptr;
  }
}

/* Query the driver's current configuration: sample rate, buffer size,
 * latencies and output channel formats. Everything comes from the driver;
 * the manufacturer's control panel is authoritative. */
bool RageSoundDriver_ASIO::InitASIO(std::string& sError) {
  // ASIO drivers may create their own windows during init; COM must be
  // available on this thread. If the game already initialized COM with a
  // different model this returns RPC_E_CHANGED_MODE, which is harmless here.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  CLSID clsid;
  std::string sDriverName;
  if (!SelectASIODriver(clsid, sDriverName, sError)) {
    return false;
  }

  m_pASIO = CreateASIOInstance(clsid, sDriverName, &m_hASIOModule);
  if (m_pASIO == nullptr) {
    sError =
        ssprintf("Failed to create ASIO instance for '%s'",
                 sDriverName.c_str());
    return false;
  }

  // The main window handle lets drivers parent dialogs (e.g. their control
  // panel) to it; some drivers refuse to initialize without one.
  HWND hWnd = GraphicsWindow::GetHwnd();
  if (!m_pASIO->vtbl->init(m_pASIO, (void*)hWnd)) {
    char szErr[256] = {0};
    m_pASIO->vtbl->getErrorMessage(m_pASIO, szErr);
    if (szErr[0] == '\0') {
      sError = ssprintf(
          "ASIO driver '%s' refused to initialize (no error message given)",
          sDriverName.c_str());
    } else {
      sError = ssprintf("ASIO init failed: %s", szErr);
    }
    ShutdownASIO();
    return false;
  }

  char szName[256] = {0};
  m_pASIO->vtbl->getDriverName(m_pASIO, szName);
  long lVersion = m_pASIO->vtbl->getDriverVersion(m_pASIO);

  // The driver's control panel owns the sample rate; adopt whatever the
  // device is currently configured to. The engine resamples all sources to
  // the rate we report from GetSampleRate().
  double dRate = 0.0;
  if (!AsioSuccess(m_pASIO->vtbl->getSampleRate(m_pASIO, &dRate)) ||
      dRate <= 0.0) {
    sError = "ASIO driver did not report a valid sample rate";
    ShutdownASIO();
    return false;
  }
  m_iSampleRate = (int)(dRate + 0.5);

  long lInputs = 0;
  m_pASIO->vtbl->getChannels(m_pASIO, &lInputs, &m_lNumOutputs);
  if (m_lNumOutputs < 2) {
    sError = ssprintf(
        "ASIO driver '%s' has %ld output channels, need at least 2",
        szName, m_lNumOutputs);
    ShutdownASIO();
    return false;
  }

  long lInputLatency = 0, lOutputLatency = 0;
  m_pASIO->vtbl->getLatencies(m_pASIO, &lInputLatency, &lOutputLatency);
  m_lOutputLatency = lOutputLatency;

  // The driver reports the buffer size its control panel is configured to
  // as the preferred size; use it as-is, aligned to the driver's rules.
  long lMin = 0, lMax = 0, lPreferred = 0, lGranularity = 0;
  if (!AsioSuccess(m_pASIO->vtbl->getBufferSize(
          m_pASIO, &lMin, &lMax, &lPreferred, &lGranularity))) {
    sError = "ASIO getBufferSize failed";
    ShutdownASIO();
    return false;
  }

  long lBufferSize = std::clamp(lPreferred, lMin, lMax);
  if (lGranularity == -1) {
    // Only powers of two are valid.
    long lPow2 = lMin > 16 ? lMin : 16;
    while (lPow2 < lBufferSize && lPow2 < lMax) {
      lPow2 *= 2;
    }
    lBufferSize = std::min(lPow2, lMax);
  } else if (lGranularity > 0) {
    lBufferSize = lMin + (lBufferSize - lMin) / lGranularity * lGranularity;
  }
  m_lBufferSize = lBufferSize;

  // Query the two output channels we drive. ITGmania mixes stereo; the
  // first output pair is the canonical stereo routing for ASIO hosts.
  for (long lChannel = 0; lChannel < 2; ++lChannel) {
    ASIOChannelInfo info = {};
    info.channel = lChannel;
    info.isInput = 0;
    if (!AsioSuccess(m_pASIO->vtbl->getChannelInfo(m_pASIO, &info))) {
      sError = ssprintf(
          "ASIO getChannelInfo failed for output channel %ld", lChannel);
      ShutdownASIO();
      return false;
    }
    info.name[31] = '\0';  // not all drivers NUL-terminate the name
    m_lSampleType[lChannel] = SanitizeSampleType(info.type, info.name);
    LOG->Info(
        "ASIO: output channel %ld is '%s', sample type %ld", lChannel,
        info.name, m_lSampleType[lChannel]);
  }

  ASIOBufferInfo infos[2] = {};
  infos[0].isInput = 0;
  infos[0].channelNum = 0;
  infos[1].isInput = 0;
  infos[1].channelNum = 1;

  ASIOError iError = m_pASIO->vtbl->createBuffers(
      m_pASIO, infos, 2, m_lBufferSize, &g_ASIOCallbacks);
  if (!AsioSuccess(iError)) {
    sError = ssprintf(
        "ASIO createBuffers failed: %s (%ld)", AsioErrorString(iError),
        iError);
    ShutdownASIO();
    return false;
  }
  m_pOutputBuffers[0][0] = infos[0].buffers[0];
  m_pOutputBuffers[0][1] = infos[0].buffers[1];
  m_pOutputBuffers[1][0] = infos[1].buffers[0];
  m_pOutputBuffers[1][1] = infos[1].buffers[1];

  m_MixScratch.resize(m_lBufferSize * 2, 0.0f);

  LOG->Info(
      "ASIO: driver '%s' v%ld, %d Hz, %ld output channels, buffer %ld "
      "frames (%.2f ms), output latency %ld frames (%.2f ms)",
      szName, lVersion, m_iSampleRate.load(), m_lNumOutputs,
      m_lBufferSize.load(), 1000.0 * m_lBufferSize / m_iSampleRate,
      m_lOutputLatency.load(), 1000.0 * m_lOutputLatency / m_iSampleRate);

  // Remember the working driver so future runs don't have to search. Save
  // immediately so the selection survives even a crash on this run.
  if (g_sASIODriverName.Get() != sDriverName) {
    g_sASIODriverName.Set(sDriverName);
    PREFSMAN->SavePrefsToDisk();
  }

  return true;
}

std::string RageSoundDriver_ASIO::Init() {
  std::string sError;
  if (!InitASIO(sError)) {
    return sError;
  }

  g_pASIODriver = this;

  /* Keep a large decode ring: dense keysound sections make the decode
   * thread spike, and with small ASIO buffers the default ring would only
   * hold a few milliseconds. */
  int iDecodeBuffer = m_lBufferSize * 4;
  if (iDecodeBuffer < 16384) {
    iDecodeBuffer = 16384;
  }
  SetDecodeBufferSize(iDecodeBuffer);
  StartDecodeThread();

  ASIOError iError = m_pASIO->vtbl->start(m_pASIO);
  if (!AsioSuccess(iError)) {
    ShutdownASIO();
    return ssprintf(
        "ASIO start failed: %s (%ld)", AsioErrorString(iError), iError);
  }
  m_bRunning = true;
  return "";
}

void RageSoundDriver_ASIO::BufferSwitch(long iDoubleBufferIndex) {
  if (!m_bRunning) {
    return;
  }

  const long lFrames = m_lBufferSize.load(std::memory_order_relaxed);
  const int64_t iHardwareFrame =
      m_iHardwareFrame.load(std::memory_order_relaxed);
  const int64_t iCurrentFrame = GetPosition();

  this->Mix(m_MixScratch.data(), lFrames, iHardwareFrame, iCurrentFrame);

  const int iIndex = (int)iDoubleBufferIndex & 1;
  const float* pMix = m_MixScratch.data();
  ConvertChannel(m_pOutputBuffers[0][iIndex], m_lSampleType[0], pMix, 2,
                 lFrames);
  ConvertChannel(m_pOutputBuffers[1][iIndex], m_lSampleType[1], pMix + 1, 2,
                 lFrames);

  // Tell the driver the output buffers are ready (ASIO 2.0 convention;
  // drivers that don't need it treat it as a no-op).
  m_pASIO->vtbl->outputReady(m_pASIO);

  m_iHardwareFrame += lFrames;
}

/* Convert interleaved float stereo mix data into one driver channel buffer.
 * Input is already clamped to [-1, 1] by the mixer. Runs on the realtime
 * thread: no allocation, no locks. */
void RageSoundDriver_ASIO::ConvertChannel(
    void* pDest, long iSampleType, const float* pMix, int iStride,
    long lFrames) {
  switch (iSampleType) {
    case ASIOSTInt16LSB: {
      int16_t* pOut = (int16_t*)pDest;
      for (long f = 0; f < lFrames; ++f) {
        pOut[f] = (int16_t)lrintf(pMix[f * iStride] * 32767.0f);
      }
      break;
    }
    case ASIOSTInt16MSB: {
      unsigned char* pOut = (unsigned char*)pDest;
      for (long f = 0; f < lFrames; ++f) {
        int16_t v = (int16_t)lrintf(pMix[f * iStride] * 32767.0f);
        pOut[f * 2 + 0] = (unsigned char)((v >> 8) & 0xFF);
        pOut[f * 2 + 1] = (unsigned char)(v & 0xFF);
      }
      break;
    }
    case ASIOSTInt24LSB: {
      unsigned char* pOut = (unsigned char*)pDest;
      for (long f = 0; f < lFrames; ++f) {
        int32_t v = (int32_t)lrintf(pMix[f * iStride] * 8388607.0f);
        pOut[f * 3 + 0] = (unsigned char)(v & 0xFF);
        pOut[f * 3 + 1] = (unsigned char)((v >> 8) & 0xFF);
        pOut[f * 3 + 2] = (unsigned char)((v >> 16) & 0xFF);
      }
      break;
    }
    case ASIOSTInt24MSB: {
      unsigned char* pOut = (unsigned char*)pDest;
      for (long f = 0; f < lFrames; ++f) {
        int32_t v = (int32_t)lrintf(pMix[f * iStride] * 8388607.0f);
        pOut[f * 3 + 0] = (unsigned char)((v >> 16) & 0xFF);
        pOut[f * 3 + 1] = (unsigned char)((v >> 8) & 0xFF);
        pOut[f * 3 + 2] = (unsigned char)(v & 0xFF);
      }
      break;
    }
    case ASIOSTInt32LSB: {
      int32_t* pOut = (int32_t*)pDest;
      for (long f = 0; f < lFrames; ++f) {
        pOut[f] = (int32_t)lrintf(pMix[f * iStride] * 2147483647.0f);
      }
      break;
    }
    case ASIOSTInt32MSB: {
      unsigned char* pOut = (unsigned char*)pDest;
      for (long f = 0; f < lFrames; ++f) {
        int32_t v = (int32_t)lrintf(pMix[f * iStride] * 2147483647.0f);
        pOut[f * 4 + 0] = (unsigned char)((v >> 24) & 0xFF);
        pOut[f * 4 + 1] = (unsigned char)((v >> 16) & 0xFF);
        pOut[f * 4 + 2] = (unsigned char)((v >> 8) & 0xFF);
        pOut[f * 4 + 3] = (unsigned char)(v & 0xFF);
      }
      break;
    }
    case ASIOSTFloat32LSB: {
      float* pOut = (float*)pDest;
      for (long f = 0; f < lFrames; ++f) {
        pOut[f] = pMix[f * iStride];
      }
      break;
    }
    case ASIOSTFloat32MSB: {
      unsigned char* pOut = (unsigned char*)pDest;
      for (long f = 0; f < lFrames; ++f) {
        union {
          float f;
          uint32_t u;
        } v;
        v.f = pMix[f * iStride];
        pOut[f * 4 + 0] = (unsigned char)((v.u >> 24) & 0xFF);
        pOut[f * 4 + 1] = (unsigned char)((v.u >> 16) & 0xFF);
        pOut[f * 4 + 2] = (unsigned char)((v.u >> 8) & 0xFF);
        pOut[f * 4 + 3] = (unsigned char)(v.u & 0xFF);
      }
      break;
    }
    case ASIOSTFloat64LSB: {
      double* pOut = (double*)pDest;
      for (long f = 0; f < lFrames; ++f) {
        pOut[f] = (double)pMix[f * iStride];
      }
      break;
    }
    case ASIOSTFloat64MSB: {
      unsigned char* pOut = (unsigned char*)pDest;
      for (long f = 0; f < lFrames; ++f) {
        union {
          double d;
          uint64_t u;
        } v;
        v.d = (double)pMix[f * iStride];
        for (int iByte = 0; iByte < 8; ++iByte) {
          pOut[f * 8 + iByte] = (unsigned char)((v.u >> (56 - iByte * 8)) & 0xFF);
        }
      }
      break;
    }
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    case ASIOSTInt32MSB16:
    case ASIOSTInt32MSB18:
    case ASIOSTInt32MSB20:
    case ASIOSTInt32MSB24: {
      // 32-bit container, significant bits left-justified.
      static const int kBits[] = {16, 18, 20, 24};
      int iIndex = (iSampleType >= ASIOSTInt32LSB16)
          ? (int)(iSampleType - ASIOSTInt32LSB16)
          : (int)(iSampleType - ASIOSTInt32MSB16);
      int iBits = kBits[iIndex];
      const bool bLSB = iSampleType >= ASIOSTInt32LSB16;
      const double dScale = (double)((1L << (iBits - 1)) - 1);
      const int iShift = 32 - iBits;
      if (bLSB) {
        int32_t* pOut = (int32_t*)pDest;
        for (long f = 0; f < lFrames; ++f) {
          pOut[f] = (int32_t)lrint(pMix[f * iStride] * dScale) << iShift;
        }
      } else {
        unsigned char* pOut = (unsigned char*)pDest;
        for (long f = 0; f < lFrames; ++f) {
          int32_t v = (int32_t)lrint(pMix[f * iStride] * dScale) << iShift;
          pOut[f * 4 + 0] = (unsigned char)((v >> 24) & 0xFF);
          pOut[f * 4 + 1] = (unsigned char)((v >> 16) & 0xFF);
          pOut[f * 4 + 2] = (unsigned char)((v >> 8) & 0xFF);
          pOut[f * 4 + 3] = (unsigned char)(v & 0xFF);
        }
      }
      break;
    }
    default:
      // Unsupported format (e.g. DSD); leave the buffer silent. This is
      // diagnosed at init time by the logged sample types.
      break;
  }
}

long RageSoundDriver_ASIO::HandleAsioMessage(long iSelector, long iValue) {
  switch (iSelector) {
    case kAsioSelectorSupported:
      // Advertise the messages we actually handle.
      switch (iValue) {
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
        case kAsioOverload:
          return 1;
        default:
          return 0;
      }
    case kAsioResetRequest:
      // The driver wants to be reinitialized. Never reconfigure underneath
      // the realtime callback; defer to Update().
      m_bReconfigureRequested = true;
      return 1;
    case kAsioBufferSizeChange:
      // The driver changed its buffer size (e.g. from its control panel).
      // Re-create buffers at the new size from Update().
      m_bReconfigureRequested = true;
      return 1;
    case kAsioResyncRequest:
      // The driver re-synchronized its clock; our position source is
      // getSamplePosition(), so there is nothing to reset.
      return 1;
    case kAsioLatenciesChanged:
      m_bLatenciesChanged = true;
      return 1;
    case kAsioOverload:
      ++m_iOverloadCount;
      return 1;
    default:
      return 0;
  }
}

void RageSoundDriver_ASIO::HandleSampleRateChange(double dRate) {
  // The hardware changed rate underneath us. The engine must pick up the
  // new rate (GetSampleRate drives the decode resampler), which requires
  // re-creating buffers; defer to Update().
  m_dReportedSampleRate = dRate;
  m_bReconfigureRequested = true;
}

void RageSoundDriver_ASIO::Update() {
  RageSoundDriver::Update();

  if (m_pASIO == nullptr) {
    return;
  }

  unsigned iOverloads = m_iOverloadCount.exchange(0);
  if (iOverloads > 0) {
    LOG->MapLog(
        "ASIOOverload", "ASIO: driver reported %u overload(s)",
        iOverloads);
  }

  if (m_bLatenciesChanged.exchange(false)) {
    long lInputLatency = 0, lOutputLatency = 0;
    if (AsioSuccess(
            m_pASIO->vtbl->getLatencies(
                m_pASIO, &lInputLatency, &lOutputLatency))) {
      m_lOutputLatency = lOutputLatency;
      LOG->Info(
          "ASIO: latencies changed, output latency now %ld frames (%.2f ms)",
          lOutputLatency, 1000.0 * lOutputLatency / m_iSampleRate);
    }
  }

  if (m_bReconfigureRequested.exchange(false)) {
    double dNewRate = m_dReportedSampleRate.exchange(-1.0);
    if (dNewRate > 0.0) {
      LOG->Info("ASIO: hardware sample rate changed to %.0f Hz", dNewRate);
    }
    if (!ReconfigureASIO()) {
      LOG->Warn("ASIO: reconfiguration failed; audio output stopped");
    }
  }
}

/* Re-create buffers at the driver's current configuration. Runs on the main
 * thread from Update(); the realtime callback is stopped while this runs. */
bool RageSoundDriver_ASIO::ReconfigureASIO() {
  m_bRunning = false;
  m_pASIO->vtbl->stop(m_pASIO);
  m_pASIO->vtbl->disposeBuffers(m_pASIO);
  m_pOutputBuffers[0][0] = m_pOutputBuffers[0][1] = nullptr;
  m_pOutputBuffers[1][0] = m_pOutputBuffers[1][1] = nullptr;

  double dRate = 0.0;
  if (AsioSuccess(m_pASIO->vtbl->getSampleRate(m_pASIO, &dRate)) &&
      dRate > 0.0) {
    m_iSampleRate = (int)(dRate + 0.5);
  }

  long lInputLatency = 0, lOutputLatency = 0;
  if (AsioSuccess(
          m_pASIO->vtbl->getLatencies(
              m_pASIO, &lInputLatency, &lOutputLatency))) {
    m_lOutputLatency = lOutputLatency;
  }

  long lMin = 0, lMax = 0, lPreferred = 0, lGranularity = 0;
  if (!AsioSuccess(m_pASIO->vtbl->getBufferSize(
          m_pASIO, &lMin, &lMax, &lPreferred, &lGranularity))) {
    return false;
  }
  long lBufferSize = std::clamp(lPreferred, lMin, lMax);
  if (lGranularity == -1) {
    long lPow2 = lMin > 16 ? lMin : 16;
    while (lPow2 < lBufferSize && lPow2 < lMax) {
      lPow2 *= 2;
    }
    lBufferSize = std::min(lPow2, lMax);
  } else if (lGranularity > 0) {
    lBufferSize = lMin + (lBufferSize - lMin) / lGranularity * lGranularity;
  }
  m_lBufferSize = lBufferSize;

  for (long lChannel = 0; lChannel < 2; ++lChannel) {
    ASIOChannelInfo info = {};
    info.channel = lChannel;
    info.isInput = 0;
    if (AsioSuccess(m_pASIO->vtbl->getChannelInfo(m_pASIO, &info))) {
      m_lSampleType[lChannel] = SanitizeSampleType(info.type, info.name);
    }
  }

  ASIOBufferInfo infos[2] = {};
  infos[0].isInput = 0;
  infos[0].channelNum = 0;
  infos[1].isInput = 0;
  infos[1].channelNum = 1;
  ASIOError iError = m_pASIO->vtbl->createBuffers(
      m_pASIO, infos, 2, m_lBufferSize, &g_ASIOCallbacks);
  if (!AsioSuccess(iError)) {
    return false;
  }
  m_pOutputBuffers[0][0] = infos[0].buffers[0];
  m_pOutputBuffers[0][1] = infos[0].buffers[1];
  m_pOutputBuffers[1][0] = infos[1].buffers[0];
  m_pOutputBuffers[1][1] = infos[1].buffers[1];

  m_MixScratch.resize(m_lBufferSize * 2, 0.0f);

  iError = m_pASIO->vtbl->start(m_pASIO);
  if (!AsioSuccess(iError)) {
    return false;
  }
  m_bRunning = true;

  LOG->Info(
      "ASIO: reconfigured, %d Hz, buffer %ld frames (%.2f ms), output "
      "latency %ld frames (%.2f ms)",
      m_iSampleRate.load(), m_lBufferSize.load(),
      1000.0 * m_lBufferSize / m_iSampleRate, m_lOutputLatency.load(),
      1000.0 * m_lOutputLatency / m_iSampleRate);
  return true;
}

int64_t RageSoundDriver_ASIO::GetPosition() const {
  if (m_pASIO == nullptr) {
    return 0;
  }
  ASIOSamples sPos = {0, 0};
  ASIOTimeStamp tStamp = {0, 0};
  if (!AsioSuccess(m_pASIO->vtbl->getSamplePosition(m_pASIO, &sPos, &tStamp))) {
    /* Don't report 0 (which would drag the clamped hardware frame
     * backwards); report the frames we have delivered so far instead. */
    return m_iHardwareFrame.load(std::memory_order_relaxed);
  }
  return ((int64_t)sPos.hi << 32) | (int64_t)sPos.lo;
}

float RageSoundDriver_ASIO::GetPlayLatency() const {
  const int iSampleRate = m_iSampleRate.load(std::memory_order_relaxed);
  if (iSampleRate <= 0) {
    return 0.0f;
  }
  return (float)m_lOutputLatency.load(std::memory_order_relaxed) /
         (float)iSampleRate;
}

int RageSoundDriver_ASIO::GetSampleRate() const {
  return m_iSampleRate.load(std::memory_order_relaxed);
}

void RageSoundDriver_ASIO::SetupDecodingThread() {
  if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  }
}
