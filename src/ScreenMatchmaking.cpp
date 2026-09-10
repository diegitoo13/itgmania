#include "ScreenMatchmaking.h"

#include <string>

#include "InputEventPlus.h"
#include "LocalizedString.h"
#include "MatchmakingManager.h"
#include "RageUtil.h"
#include "ScreenDimensions.h"
#include "ScreenManager.h"
#include "ScreenMessage.h"
#include "ThemeManager.h"

static const float MATCHED_COUNTDOWN_SECONDS = 5.0f;
static const float SEARCH_TIMEOUT_SECONDS = 300.0f;
static const float MATCHED_IMPACT_SECONDS = 0.85f;
static const float MATCHED_SWOOSH_AT_SECONDS = 0.4f;
static const float WAITING_LOOP_DELAY_SECONDS = 1.0f;

REGISTER_SCREEN_CLASS(ScreenMatchmaking);

void ScreenMatchmaking::Init() {
  ScreenWithMenuElements::Init();

  m_fMatchedCountdown = 0.0f;
  m_bWindupPlayed = false;
  m_bImpactPlayed = false;
  m_bGameplayPrepared = false;
  m_bProceeding = false;

  std::string sResumeScreen = MATCHMAKING->GetResumeScreen();
  if (sResumeScreen.empty()) {
    sResumeScreen = "ScreenStageInformation";
  }
  SetNextScreenName(sResumeScreen);

  // All visuals live in the theme overlay (BGAnimations/ScreenMatchmaking
  // overlay); this screen only drives flow and sound.
  m_soundEnqueue.Load(THEME->GetPathS("ScreenMatchmaking", "enqueue"));
  m_soundWaitingLoop.Load(THEME->GetPathS("ScreenMatchmaking", "waiting-loop"));
  m_soundWindup.Load(THEME->GetPathS("ScreenMatchmaking", "windup"));
  m_soundImpact.Load(THEME->GetPathS("ScreenMatchmaking", "impact"));
  m_soundSwoosh.Load(THEME->GetPathS("ScreenMatchmaking", "swoosh"));
}

void ScreenMatchmaking::BeginScreen() {
  ScreenWithMenuElements::BeginScreen();
  m_SearchTimer.Touch();
  PlaySearchSounds();
}

void ScreenMatchmaking::PlaySearchSounds() {
  m_WaitingLoopTimer.Touch();
  if (!m_soundEnqueue.GetLoadedFilePath().empty()) {
    m_soundEnqueue.Play(false);
  }
}

void ScreenMatchmaking::PlayMatchedSounds() {
  m_soundWaitingLoop.StopPlaying();
  if (!m_soundWindup.GetLoadedFilePath().empty()) {
    m_soundWindup.Play(false);
  }
  m_bWindupPlayed = true;
}

void ScreenMatchmaking::Update(float fDeltaTime) {
  ScreenWithMenuElements::Update(fDeltaTime);

  const MatchmakingManager::SearchState state = MATCHMAKING->GetState();
  const bool bSearching =
      state == MatchmakingManager::SearchState_Connecting ||
      state == MatchmakingManager::SearchState_Queued;

  // Waiting loop: starts shortly after entering and re-triggers at a lower
  // volume each pass (RageSound volume is set per Play), so long searches
  // gradually go quiet. Any state change kills it.
  if (bSearching &&
      m_WaitingLoopTimer.Ago() >= WAITING_LOOP_DELAY_SECONDS &&
      !m_soundWaitingLoop.GetLoadedFilePath().empty()) {
    if (!m_soundWaitingLoop.IsPlaying()) {
      // Full volume for the first pass, then stepped down each loop.
      static const float kVolumes[] = {1.0f, 0.45f, 0.2f, 0.08f, 0.04f};
      float fPlayingFor =
          m_WaitingLoopTimer.Ago() - WAITING_LOOP_DELAY_SECONDS;
      int iPass = (int)(fPlayingFor / 10.0f);
      iPass = std::min(iPass, (int)(sizeof(kVolumes) / sizeof(float)) - 1);
      RageSoundParams params;
      params.m_Volume = kVolumes[iPass];
      m_soundWaitingLoop.Play(false, &params);
    }
  } else if (m_soundWaitingLoop.IsPlaying()) {
    m_soundWaitingLoop.StopPlaying();
  }

  if (m_bProceeding) {
    return;
  }

  switch (state) {
    case MatchmakingManager::SearchState_Connecting:
    case MatchmakingManager::SearchState_Queued:
      if (m_SearchTimer.Ago() > SEARCH_TIMEOUT_SECONDS) {
        MATCHMAKING->CancelSearch();
        ProceedToResumeScreen();
      }
      break;
    case MatchmakingManager::SearchState_Matched: {
      if (!m_bWindupPlayed) {
        PlayMatchedSounds();
      }
      // Build gameplay in the background of the VS intro so the handoff is
      // instant (this is what ScreenStageInformation normally hides). The
      // opponent is already joined at this point, so the prepared screen
      // has both players.
      if (!m_bGameplayPrepared) {
        m_bGameplayPrepared = true;
        SCREENMAN->PrepareScreen("ScreenGameplay");
      }
      if (m_fMatchedCountdown == 0.0f) {
        m_fMatchedCountdown = MATCHED_COUNTDOWN_SECONDS;
      }
      const float fElapsed = MATCHED_COUNTDOWN_SECONDS - m_fMatchedCountdown;
      if (!m_bImpactPlayed && fElapsed >= MATCHED_IMPACT_SECONDS) {
        if (!m_soundImpact.GetLoadedFilePath().empty()) {
          m_soundImpact.Play(false);
        }
        m_bImpactPlayed = true;
      }
      const float fRemaining = m_fMatchedCountdown;
      if (fRemaining <= MATCHED_SWOOSH_AT_SECONDS &&
          !m_soundSwoosh.GetLoadedFilePath().empty() &&
          !m_soundSwoosh.IsPlaying()) {
        m_soundSwoosh.Play(false);
      }
      m_fMatchedCountdown -= fDeltaTime;
      if (m_fMatchedCountdown <= 0.0f) {
        MATCHMAKING->BeginPlaying();
        ProceedToResumeScreen();
      }
      break;
    }
    case MatchmakingManager::SearchState_Idle:
      // The search was cancelled or failed; continue solo.
      ProceedToResumeScreen();
      break;
    default:
      break;
  }
}

void ScreenMatchmaking::ProceedToResumeScreen() {
  if (m_bProceeding || IsTransitioning()) {
    return;
  }
  m_bProceeding = true;
  m_soundWaitingLoop.StopPlaying();
  StartTransitioningScreen(SM_GoToNextScreen);
}

bool ScreenMatchmaking::Input(const InputEventPlus& input) {
  if (IsTransitioning()) {
    return true;
  }

  if (input.type != IET_FIRST_PRESS) {
    return ScreenWithMenuElements::Input(input);
  }

  switch (input.MenuI) {
    case GAME_BUTTON_START:
      // Give up on the search and play solo.
      MATCHMAKING->CancelSearch();
      ProceedToResumeScreen();
      return true;
    case GAME_BUTTON_BACK:
      MATCHMAKING->CancelSearch();
      Cancel(SM_GoToPrevScreen);
      return true;
    default:
      break;
  }

  return ScreenWithMenuElements::Input(input);
}
