/* ScreenMatchmaking - Waits for a network opponent before gameplay. */

#ifndef SCREEN_MATCHMAKING_H
#define SCREEN_MATCHMAKING_H

#include "RageSound.h"
#include "RageTimer.h"
#include "ScreenWithMenuElements.h"

class ScreenMatchmaking : public ScreenWithMenuElements {
 public:
  virtual void Init();
  virtual void BeginScreen();
  virtual void Update(float fDeltaTime);
  virtual bool Input(const InputEventPlus& input);

 private:
  void ProceedToResumeScreen();
  void PlaySearchSounds();
  void PlayMatchedSounds();

  RageSound m_soundEnqueue;
  RageSound m_soundWaitingLoop;
  RageSound m_soundWindup;
  RageSound m_soundImpact;
  RageSound m_soundSwoosh;

  RageTimer m_SearchTimer;
  RageTimer m_WaitingLoopTimer;
  float m_fMatchedCountdown;
  bool m_bWindupPlayed;
  bool m_bImpactPlayed;
  bool m_bGameplayPrepared;
  bool m_bProceeding;
};

#endif
