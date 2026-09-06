#ifndef RAGE_SOUND_WASAPI_H
#define RAGE_SOUND_WASAPI_H

// clang-format off
#include <windows.h>
#include <mmreg.h>
// clang-format on

#include <atomic>
#include <vector>

#include "RageSoundDriver.h"
#include "RageThreads.h"

struct IAudioClient;
struct IAudioRenderClient;
struct IAudioClock;

class RageSoundDriver_WASAPI : public RageSoundDriver {
 public:
  RageSoundDriver_WASAPI();
  virtual ~RageSoundDriver_WASAPI();
  std::string Init() override;

  int64_t GetPosition() const override;
  float GetPlayLatency() const override;
  int GetSampleRate() const override;

 protected:
  void SetupDecodingThread() override;

 private:
  int m_iSampleRate;
  int m_iChannels;
  UINT32 m_iBufferSizeFrames;
  bool m_bFloat;  // true if we are using float, false if 16-bit PCM
  bool m_bExclusive;  // true if using exclusive-mode WASAPI
  bool m_bLowLatencyShared;  // true if using IAudioClient3 small periods

  // Scratch stereo buffer used when the device runs with more than 2
  // channels: we mix stereo, then expand into the device buffer.
  std::vector<float> m_MixScratchFloat;
  std::vector<int16_t> m_MixScratchInt;

  IAudioClient* m_pAudioClient;
  IAudioRenderClient* m_pRenderClient;
  IAudioClock* m_pAudioClock;
  UINT64 m_iAudioClockFreq;  // device units per second, from GetFrequency

  HANDLE m_hAudioEvent;

  std::atomic<bool> m_bShutdownMixerThread;
  bool m_bOwnsComInit;

  static int MixerThread_start(void* p);
  void MixerThread();
  RageThread m_MixingThread;

  bool InitWASAPI(std::string& sError);
  HRESULT TryInitialize(int iShareMode, WAVEFORMATEX* pwfx,
                        long long hnsDuration);
  void FreeWASAPI();
};

#endif
