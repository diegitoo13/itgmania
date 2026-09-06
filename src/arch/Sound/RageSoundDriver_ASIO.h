#ifndef RAGE_SOUND_DRIVER_ASIO_H
#define RAGE_SOUND_DRIVER_ASIO_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "RageSoundDriver.h"
#include "RageThreads.h"

struct IASIO;

/*
 * Generic ASIO output driver.
 *
 * The backend is device-agnostic: it enumerates the ASIO drivers installed
 * on the system (HKLM\SOFTWARE\ASIO), loads one, and adopts whatever the
 * driver's control panel is configured to (sample rate, buffer size, sample
 * format, channel layout). ITGmania deliberately has no preferences for
 * those values; the manufacturer driver is authoritative. The only ASIO
 * preference is "ASIODriver", which selects which installed driver to use
 * and is remembered after the first successful initialization.
 */
class RageSoundDriver_ASIO : public RageSoundDriver {
 public:
  RageSoundDriver_ASIO();
  virtual ~RageSoundDriver_ASIO();
  std::string Init() override;

  int64_t GetPosition() const override;
  float GetPlayLatency() const override;
  int GetSampleRate() const override;

  void Update() override;

  /* Called from the ASIO driver's realtime thread. Must not allocate, lock,
   * or block. */
  void BufferSwitch(long iDoubleBufferIndex);

  /* Called from the ASIO driver's realtime thread on driver messages and
   * sample rate changes. Only defers work to Update(); returns the reply
   * value for asioMessage. */
  long HandleAsioMessage(long iSelector, long iValue);
  void HandleSampleRateChange(double dRate);

 protected:
  void SetupDecodingThread() override;

 private:
  bool InitASIO(std::string& sError);
  void ShutdownASIO();

  /* Safe reconfiguration, executed from Update() (never from the realtime
   * callback): stop the driver, dispose and re-create buffers at the
   * driver's current configuration, restart. */
  bool ReconfigureASIO();
  void FillOutput(long iDoubleBufferIndex);

  /* Write iFrames stereo frames from pMix (interleaved float, [-1,1]) into
   * one channel buffer in the driver's sample format. */
  static void ConvertChannel(
      void* pDest, long iSampleType, const float* pMix, int iStride,
      long iFrames);

  IASIO* m_pASIO;
  void* m_hASIOModule;  // HMODULE when loaded through the fallback path

  std::atomic<int> m_iSampleRate;
  std::atomic<long> m_lBufferSize;     // frames per buffer switch
  std::atomic<long> m_lOutputLatency;  // frames, driver-reported
  long m_lSampleType[2];               // sample type of each output channel
  long m_lNumOutputs;                  // total device output channels

  // Scratch interleaved stereo mix buffer, written by the realtime callback.
  std::vector<float> m_MixScratch;

  // Pointers into the driver's double buffers for the two output channels.
  void* m_pOutputBuffers[2][2];

  std::atomic<int64_t> m_iHardwareFrame;  // buffers delivered to the driver

  // Reconfiguration requests, set by the realtime callback (driver messages)
  // and handled by Update().
  std::atomic<bool> m_bReconfigureRequested;
  std::atomic<bool> m_bLatenciesChanged;
  std::atomic<unsigned> m_iOverloadCount;
  std::atomic<double> m_dReportedSampleRate;  // last rate announced by driver

  std::atomic<bool> m_bRunning;
};

#endif
