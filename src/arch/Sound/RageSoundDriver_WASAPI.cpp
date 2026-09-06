#include "RageSoundDriver_WASAPI.h"

// clang-format off
#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
// clang-format on

#include "PrefsManager.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "archutils/Win32/DirectXHelpers.h"
#include "archutils/Win32/ErrorStrings.h"
#include "global.h"

REGISTER_SOUND_DRIVER_CLASS2("WASAPI", WASAPI);

/* When true (default), WASAPI prefers exclusive mode for the lowest output
 * latency, bypassing the Windows audio engine. Set to 0 to stay in shared
 * mode: the engine mixes the game with the rest of the desktop, so system
 * capture (OBS desktop audio, NDI Screen Capture, ...) can hear it. The
 * low-latency shared path (IAudioClient3) is still used when available. */
static Preference<bool> g_bWASAPIExclusive("WASAPIExclusive", true);

RageSoundDriver_WASAPI::RageSoundDriver_WASAPI()
    : m_iSampleRate(0),
      m_iChannels(2),
      m_iBufferSizeFrames(0),
      m_bFloat(true),
      m_bExclusive(false),
      m_bLowLatencyShared(false),
      m_pAudioClient(nullptr),
      m_pRenderClient(nullptr),
      m_pAudioClock(nullptr),
      m_iAudioClockFreq(0),
      m_hAudioEvent(INVALID_HANDLE_VALUE),
      m_bShutdownMixerThread(false),
      m_bOwnsComInit(false) {}

RageSoundDriver_WASAPI::~RageSoundDriver_WASAPI() {
  if (m_MixingThread.IsCreated()) {
    m_bShutdownMixerThread = true;
    if (m_hAudioEvent != INVALID_HANDLE_VALUE) {
      SetEvent(m_hAudioEvent);  // Wake up thread
    }
    m_MixingThread.Wait();
  }

  FreeWASAPI();

  if (m_hAudioEvent != INVALID_HANDLE_VALUE) {
    CloseHandle(m_hAudioEvent);
  }
}

void RageSoundDriver_WASAPI::FreeWASAPI() {
  if (m_pAudioClient) {
    m_pAudioClient->Stop();
  }
  if (m_pAudioClock) {
    m_pAudioClock->Release();
    m_pAudioClock = nullptr;
  }
  if (m_pRenderClient) {
    m_pRenderClient->Release();
    m_pRenderClient = nullptr;
  }
  if (m_pAudioClient) {
    m_pAudioClient->Release();
    m_pAudioClient = nullptr;
  }
  if (m_bOwnsComInit) {
    CoUninitialize();
    m_bOwnsComInit = false;
  }
}

namespace {

// We can only mix into 32-bit float or 16-bit PCM.
bool IsSupportedWaveFormat(const WAVEFORMATEX* pwfx, bool& bFloatOut) {
  if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const WAVEFORMATEXTENSIBLE* pEx =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
    if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
      bFloatOut = true;
      return true;
    }
    if (pEx->SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
        pEx->Format.wBitsPerSample == 16) {
      bFloatOut = false;
      return true;
    }
    return false;
  }
  if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
      pwfx->wBitsPerSample == 32) {
    bFloatOut = true;
    return true;
  }
  if (pwfx->wFormatTag == WAVE_FORMAT_PCM && pwfx->wBitsPerSample == 16) {
    bFloatOut = false;
    return true;
  }
  return false;
}

}  // namespace

HRESULT RageSoundDriver_WASAPI::TryInitialize(
    int iShareMode, WAVEFORMATEX* pwfx, long long hnsDuration) {
  const AUDCLNT_SHAREMODE shareMode =
      static_cast<AUDCLNT_SHAREMODE>(iShareMode);
  // In exclusive mode with event-driven buffering, the period must equal
  // the buffer duration.
  const REFERENCE_TIME hnsPeriodicity =
      shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE ? hnsDuration : 0;
  HRESULT hr = m_pAudioClient->Initialize(
      shareMode, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsDuration,
      hnsPeriodicity, pwfx, nullptr);
  if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
    // The driver rejected our buffer size; ask for the required alignment
    // and retry once with it.
    UINT32 iRequiredFrames = 0;
    HRESULT hr2 = m_pAudioClient->GetBufferSize(&iRequiredFrames);
    if (SUCCEEDED(hr2) && iRequiredFrames > 0) {
      REFERENCE_TIME hnsAligned =
          ((REFERENCE_TIME)iRequiredFrames * 10000000LL +
           pwfx->nSamplesPerSec - 1) /
          pwfx->nSamplesPerSec;
      const REFERENCE_TIME hnsAlignedPeriodicity =
          shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE ? hnsAligned : 0;
      hr = m_pAudioClient->Initialize(
          shareMode, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsAligned,
          hnsAlignedPeriodicity, pwfx, nullptr);
    }
  }
  return hr;
}

bool RageSoundDriver_WASAPI::InitWASAPI(std::string& sError) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    sError = hr_ssprintf(hr, "CoInitializeEx failed");
    return false;
  }
  m_bOwnsComInit = SUCCEEDED(hr);

  IMMDeviceEnumerator* pEnumerator = nullptr;
  hr = CoCreateInstance(
      __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
      IID_PPV_ARGS(&pEnumerator));
  if (FAILED(hr)) {
    sError = hr_ssprintf(hr, "CoCreateInstance(MMDeviceEnumerator) failed");
    FreeWASAPI();
    return false;
  }

  IMMDevice* pDevice = nullptr;
  hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
  pEnumerator->Release();
  if (FAILED(hr)) {
    sError = hr_ssprintf(hr, "GetDefaultAudioEndpoint failed");
    FreeWASAPI();
    return false;
  }

  hr = pDevice->Activate(
      __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_pAudioClient);
  pDevice->Release();
  if (FAILED(hr)) {
    sError = hr_ssprintf(hr, "Activate(IAudioClient) failed");
    FreeWASAPI();
    return false;
  }

  WAVEFORMATEX* pwfx = nullptr;
  hr = m_pAudioClient->GetMixFormat(&pwfx);
  if (FAILED(hr)) {
    sError = hr_ssprintf(hr, "GetMixFormat failed");
    FreeWASAPI();
    return false;
  }

  REFERENCE_TIME hnsDefaultPeriod = 0, hnsMinimumPeriod = 0;
  m_pAudioClient->GetDevicePeriod(&hnsDefaultPeriod, &hnsMinimumPeriod);

  int iChannels = pwfx->nChannels;
  const REFERENCE_TIME aDurations[] = {
      hnsMinimumPeriod, hnsMinimumPeriod * 2, hnsMinimumPeriod * 3,
      hnsDefaultPeriod};

  // Prefer exclusive mode: it bypasses the Windows audio engine entirely
  // and can run at the device's minimum period, which is several times
  // lower latency than shared mode. Skipped entirely when the user asked
  // for shared mode (WASAPIExclusive=0) so system capture can hear the game.
  const bool bTryExclusive = g_bWASAPIExclusive.Get();
  bool bFormatFloat = true;
  if (!bTryExclusive) {
    LOG->Info(
        "WASAPI: exclusive mode disabled by preference (WASAPIExclusive=0)");
  } else if (IsSupportedWaveFormat(pwfx, bFormatFloat)) {
    for (REFERENCE_TIME hnsDur : aDurations) {
      if (hnsDur <= 0) {
        continue;
      }
      HRESULT hrEx = TryInitialize(AUDCLNT_SHAREMODE_EXCLUSIVE, pwfx, hnsDur);
      if (SUCCEEDED(hrEx)) {
        m_bExclusive = true;
        m_bFloat = bFormatFloat;
        m_iSampleRate = pwfx->nSamplesPerSec;
        iChannels = pwfx->nChannels;
        break;
      }
      LOG->Info(
          "WASAPI: exclusive init with mix format (%d ch, %d Hz) at "
          "%.2f ms failed: %s",
          pwfx->nChannels, pwfx->nSamplesPerSec, hnsDur / 10000.0,
          hr_ssprintf(hrEx, "Initialize failed").c_str());
    }
  } else {
    LOG->Info(
        "WASAPI: mix format not mixable (tag %d, %d bits), skipping "
        "exclusive attempt with it",
        pwfx->wFormatTag, pwfx->wBitsPerSample);
  }

  if (!m_bExclusive && bTryExclusive) {
    // The mix format was rejected in exclusive mode. Probe raw formats,
    // keeping the device's channel count: interface drivers usually only
    // accept their native channel layout in exclusive mode.
    const int iMixChannels = pwfx->nChannels;
    const struct {
      int iRate;
      bool bFloat;
      int iChannels;
    } aCandidates[] = {
        {48000, true, iMixChannels},  {48000, false, iMixChannels},
        {48000, false, 2},            {48000, true, 2},
        {44100, true, iMixChannels},  {44100, false, iMixChannels},
        {44100, false, 2},            {44100, true, 2},
    };
    for (const auto& candidate : aCandidates) {
      WAVEFORMATEXTENSIBLE fmt = {};
      fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      fmt.Format.nChannels = (WORD)candidate.iChannels;
      fmt.Format.nSamplesPerSec = candidate.iRate;
      fmt.Format.wBitsPerSample = candidate.bFloat ? 32 : 16;
      fmt.Format.nBlockAlign =
          fmt.Format.nChannels * fmt.Format.wBitsPerSample / 8;
      fmt.Format.nAvgBytesPerSec = candidate.iRate * fmt.Format.nBlockAlign;
      fmt.Format.cbSize = 22;
      fmt.Samples.wValidBitsPerSample = fmt.Format.wBitsPerSample;
      fmt.dwChannelMask =
          candidate.iChannels > 2
              ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                 SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                 SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                 SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
              : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
      fmt.SubFormat = candidate.bFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                       : KSDATAFORMAT_SUBTYPE_PCM;

      WAVEFORMATEX* pUse = reinterpret_cast<WAVEFORMATEX*>(&fmt);
      WAVEFORMATEX* pClosest = nullptr;
      HRESULT hrSupport = m_pAudioClient->IsFormatSupported(
          AUDCLNT_SHAREMODE_EXCLUSIVE, pUse, &pClosest);
      bool bOwnsFormat = false;
      if (hrSupport == S_FALSE && pClosest != nullptr) {
        pUse = pClosest;
        bOwnsFormat = true;
      } else if (hrSupport != S_OK) {
        LOG->Info(
            "WASAPI: exclusive %d ch %d Hz %s rejected by "
            "IsFormatSupported: %s",
            candidate.iChannels, candidate.iRate,
            candidate.bFloat ? "Float" : "Int16",
            hr_ssprintf(hrSupport, "not supported").c_str());
        if (pClosest != nullptr) {
          CoTaskMemFree(pClosest);
        }
        continue;
      }

      bool bCandidateFloat = candidate.bFloat;
      bool bUsable = IsSupportedWaveFormat(pUse, bCandidateFloat);
      if (bUsable) {
        for (REFERENCE_TIME hnsDur : aDurations) {
          if (hnsDur <= 0) {
            continue;
          }
          HRESULT hrEx =
              TryInitialize(AUDCLNT_SHAREMODE_EXCLUSIVE, pUse, hnsDur);
          if (SUCCEEDED(hrEx)) {
            m_bExclusive = true;
            m_bFloat = bCandidateFloat;
            m_iSampleRate = pUse->nSamplesPerSec;
            iChannels = pUse->nChannels;
            break;
          }
          LOG->Info(
              "WASAPI: exclusive init with %d ch %d Hz %s at %.2f ms "
              "failed: %s",
              pUse->nChannels, pUse->nSamplesPerSec,
              bCandidateFloat ? "Float" : "Int16", hnsDur / 10000.0,
              hr_ssprintf(hrEx, "Initialize failed").c_str());
        }
      } else {
        LOG->Info(
            "WASAPI: %d ch %d Hz %s candidate not usable for mixing, "
            "skipped",
            pUse->nChannels, pUse->nSamplesPerSec,
            candidate.bFloat ? "Float" : "Int16");
      }
      if (bOwnsFormat) {
        CoTaskMemFree(pUse);
      }
      if (m_bExclusive) {
        break;
      }
    }
  }

  if (!m_bExclusive) {
    // Exclusive mode failed. Next best: IAudioClient3 low-latency shared
    // mode, which lets the audio engine run with much smaller periods
    // than the classic shared-mode default when the driver supports it.
    IAudioClient3* pClient3 = nullptr;
    HRESULT hr3 = m_pAudioClient->QueryInterface(
        __uuidof(IAudioClient3), (void**)&pClient3);
    if (SUCCEEDED(hr3) && pClient3 != nullptr) {
      UINT32 iDefaultPeriod = 0, iFundamentalPeriod = 0, iMinPeriod = 0,
             iMaxPeriod = 0;
      hr3 = pClient3->GetSharedModeEnginePeriod(
          pwfx, &iDefaultPeriod, &iFundamentalPeriod, &iMinPeriod,
          &iMaxPeriod);
      if (SUCCEEDED(hr3)) {
        LOG->Info(
            "WASAPI: engine periods (frames): default %d, fundamental %d, "
            "min %d, max %d",
            iDefaultPeriod, iFundamentalPeriod, iMinPeriod, iMaxPeriod);
        if (!IsSupportedWaveFormat(pwfx, bFormatFloat)) {
          sError = "Unsupported mix format (expected float or 16-bit PCM)";
          pClient3->Release();
          CoTaskMemFree(pwfx);
          FreeWASAPI();
          return false;
        }
        hr3 = pClient3->InitializeSharedAudioStream(
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, iMinPeriod, pwfx, nullptr);
        if (SUCCEEDED(hr3)) {
          m_bLowLatencyShared = true;
          m_bFloat = bFormatFloat;
          m_iSampleRate = pwfx->nSamplesPerSec;
          iChannels = pwfx->nChannels;
        } else {
          LOG->Info(
              "WASAPI: InitializeSharedAudioStream at %d frames failed: %s",
              iMinPeriod,
              hr_ssprintf(hr3, "InitializeSharedAudioStream failed")
                  .c_str());
        }
      } else {
        LOG->Info(
            "WASAPI: GetSharedModeEnginePeriod failed: %s",
            hr_ssprintf(hr3, "GetSharedModeEnginePeriod failed").c_str());
      }
      pClient3->Release();
    }
  }

  if (!m_bExclusive && !m_bLowLatencyShared) {
    // Shared-mode fallback (original behavior).
    if (!IsSupportedWaveFormat(pwfx, bFormatFloat)) {
      sError = "Unsupported mix format (expected float or 16-bit PCM)";
      CoTaskMemFree(pwfx);
      FreeWASAPI();
      return false;
    }
    m_bFloat = bFormatFloat;
    m_iSampleRate = pwfx->nSamplesPerSec;
    iChannels = pwfx->nChannels;

    REFERENCE_TIME hnsRequestedDuration = 0;
    if (PREFSMAN->m_iSoundWriteAhead) {
      // WriteAhead is in frames. Convert to 100-nanosecond units.
      hnsRequestedDuration = (REFERENCE_TIME)PREFSMAN->m_iSoundWriteAhead *
                             10000000ULL / m_iSampleRate;
    }

    hr = TryInitialize(AUDCLNT_SHAREMODE_SHARED, pwfx, hnsRequestedDuration);
    if (FAILED(hr)) {
      sError = hr_ssprintf(hr, "Initialize(IAudioClient) failed");
      CoTaskMemFree(pwfx);
      FreeWASAPI();
      return false;
    }
  }
  CoTaskMemFree(pwfx);

  m_hAudioEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (m_hAudioEvent == NULL) {
    sError = werr_ssprintf(GetLastError(), "CreateEvent failed");
    FreeWASAPI();
    return false;
  }

  hr = m_pAudioClient->SetEventHandle(m_hAudioEvent);
  if (FAILED(hr)) {
    sError = hr_ssprintf(hr, "SetEventHandle failed");
    FreeWASAPI();
    return false;
  }

  hr = m_pAudioClient->GetBufferSize(&m_iBufferSizeFrames);
  if (FAILED(hr)) {
    sError = hr_ssprintf(hr, "GetBufferSize failed");
    FreeWASAPI();
    return false;
  }

  hr = m_pAudioClient->GetService(IID_PPV_ARGS(&m_pRenderClient));
  if (FAILED(hr)) {
    sError = hr_ssprintf(hr, "GetService(IAudioRenderClient) failed");
    FreeWASAPI();
    return false;
  }

  hr = m_pAudioClient->GetService(IID_PPV_ARGS(&m_pAudioClock));
  if (FAILED(hr)) {
    sError = hr_ssprintf(hr, "GetService(IAudioClock) failed");
    FreeWASAPI();
    return false;
  }

  hr = m_pAudioClock->GetFrequency(&m_iAudioClockFreq);
  if (FAILED(hr)) {
    sError = hr_ssprintf(hr, "GetFrequency failed");
    FreeWASAPI();
    return false;
  }

  REFERENCE_TIME hnsStreamLatency = 0;
  m_pAudioClient->GetStreamLatency(&hnsStreamLatency);

  m_iChannels = iChannels;
  if (m_iChannels > 2) {
    // We mix stereo internally; allocate scratch buffers to expand into
    // the device's channel layout.
    m_MixScratchFloat.resize(m_iBufferSizeFrames * 2, 0.0f);
    m_MixScratchInt.resize(m_iBufferSizeFrames * 2, 0);
  }

  LOG->Info(
      "WASAPI: %s mode, %d channels, %d Hz, %s, buffer size %d frames "
      "(%.2f ms), device period min %.2f ms default %.2f ms, stream "
      "latency %.2f ms",
      m_bExclusive ? "Exclusive"
                   : (m_bLowLatencyShared ? "Low-latency shared" : "Shared"),
      iChannels, m_iSampleRate, m_bFloat ? "Float" : "Int16",
      m_iBufferSizeFrames, 1000.0 * m_iBufferSizeFrames / m_iSampleRate,
      hnsMinimumPeriod / 10000.0, hnsDefaultPeriod / 10000.0,
      hnsStreamLatency / 10000.0);

  return true;
}

std::string RageSoundDriver_WASAPI::Init() {
  std::string sError;
  if (!InitWASAPI(sError)) {
    return sError;
  }

  // Set decode buffer size.
  // We want it to be at least as big as the WASAPI buffer, but keep a
  // healthy floor: with a tiny exclusive-mode buffer the default size
  // would be only a few milliseconds and the decode thread could starve
  // the mixer.
  int iDecodeBuffer = m_iBufferSizeFrames * 3 / 2;
  if (iDecodeBuffer < 8192) {
    iDecodeBuffer = 8192;
  }
  SetDecodeBufferSize(iDecodeBuffer);
  StartDecodeThread();

  m_MixingThread.SetName("WASAPI Mixer Thread");
  m_MixingThread.Create(MixerThread_start, this);

  return "";
}

int RageSoundDriver_WASAPI::MixerThread_start(void* p) {
  ((RageSoundDriver_WASAPI*)p)->MixerThread();
  return 0;
}

void RageSoundDriver_WASAPI::MixerThread() {
  if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
      LOG->Warn(
          werr_ssprintf(
              GetLastError(), "Failed to set WASAPI sound thread priority")
              .c_str());
    }
  }

  HRESULT hr = m_pAudioClient->Start();
  if (FAILED(hr)) {
    LOG->Warn(hr_ssprintf(hr, "Failed to start IAudioClient").c_str());
    return;
  }

  {
    REFERENCE_TIME hnsLatency = 0;
    if (SUCCEEDED(m_pAudioClient->GetStreamLatency(&hnsLatency))) {
      LOG->Info(
          "WASAPI: stream started, measured output latency %.2f ms",
          hnsLatency / 10000.0);
    }
  }

  int64_t iHardwareFrame = 0;  // Total frames played/mixed so far

  while (!m_bShutdownMixerThread) {
    DWORD waitResult =
        WaitForSingleObject(m_hAudioEvent, 2000);  // 2 second timeout safety
    if (waitResult == WAIT_TIMEOUT) {
      LOG->Warn("WASAPI: Timeout waiting for audio event");
      m_pAudioClient->Stop();
      m_pAudioClient->Start();
      continue;
    }

    if (m_bShutdownMixerThread) {
      break;
    }

    UINT32 padding = 0;
    hr = m_pAudioClient->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
      LOG->Warn(hr_ssprintf(hr, "GetCurrentPadding failed").c_str());
      continue;
    }

    UINT32 framesAvailable = m_iBufferSizeFrames - padding;
    if (framesAvailable == 0) {
      continue;
    }

    BYTE* pData = nullptr;
    hr = m_pRenderClient->GetBuffer(framesAvailable, &pData);
    if (FAILED(hr)) {
      LOG->Warn(hr_ssprintf(hr, "GetBuffer failed").c_str());
      continue;
    }

    int64_t iCurrentFrame = GetPosition();

    if (m_iChannels == 2) {
      if (m_bFloat) {
        this->Mix(
            (float*)pData, framesAvailable, iHardwareFrame, iCurrentFrame);
      } else {
        this->Mix(
            (int16_t*)pData, framesAvailable, iHardwareFrame, iCurrentFrame);
      }
    } else {
      // The device runs with more than 2 channels (e.g. an audio
      // interface exposing 8 channels). Mix stereo into scratch, then
      // expand: stereo goes to channels 1-2, all other channels silent.
      if (m_bFloat) {
        this->Mix(
            m_MixScratchFloat.data(), framesAvailable, iHardwareFrame,
            iCurrentFrame);
        float* pOut = (float*)pData;
        memset(pOut, 0, framesAvailable * m_iChannels * sizeof(float));
        for (UINT32 f = 0; f < framesAvailable; ++f) {
          pOut[f * m_iChannels + 0] = m_MixScratchFloat[f * 2 + 0];
          pOut[f * m_iChannels + 1] = m_MixScratchFloat[f * 2 + 1];
        }
      } else {
        this->Mix(
            m_MixScratchInt.data(), framesAvailable, iHardwareFrame,
            iCurrentFrame);
        int16_t* pOut = (int16_t*)pData;
        memset(pOut, 0, framesAvailable * m_iChannels * sizeof(int16_t));
        for (UINT32 f = 0; f < framesAvailable; ++f) {
          pOut[f * m_iChannels + 0] = m_MixScratchInt[f * 2 + 0];
          pOut[f * m_iChannels + 1] = m_MixScratchInt[f * 2 + 1];
        }
      }
    }

    hr = m_pRenderClient->ReleaseBuffer(framesAvailable, 0);
    if (FAILED(hr)) {
      LOG->Warn(hr_ssprintf(hr, "ReleaseBuffer failed").c_str());
    }

    iHardwareFrame += framesAvailable;
  }

  m_pAudioClient->Stop();
}

int64_t RageSoundDriver_WASAPI::GetPosition() const {
  UINT64 position = 0;
  if (FAILED(m_pAudioClock->GetPosition(&position, nullptr))) {
    return 0;
  }
  return (int64_t)(position * (UINT64)m_iSampleRate / m_iAudioClockFreq);
}

float RageSoundDriver_WASAPI::GetPlayLatency() const {
  REFERENCE_TIME latency = 0;
  if (FAILED(m_pAudioClient->GetStreamLatency(&latency))) {
    return 0.0f;
  }
  return (float)latency / 10000000.0f;
}

int RageSoundDriver_WASAPI::GetSampleRate() const { return m_iSampleRate; }

void RageSoundDriver_WASAPI::SetupDecodingThread() {
  if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
    LOG->Warn(
        werr_ssprintf(
            GetLastError(), "Failed to set WASAPI decoding thread priority")
            .c_str());
  }
}
