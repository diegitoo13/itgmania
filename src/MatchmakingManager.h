#ifndef MATCHMAKING_MANAGER_H
#define MATCHMAKING_MANAGER_H

#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXWebSocket.h>

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "GameConstantsAndTypes.h"
#include "PlayerNumber.h"
#include "Preference.h"
#include "RageTimer.h"

struct lua_State;
namespace Json {
class Value;
}

struct NetworkJudgmentEvent {
  int row = 0;
  int col = 0;
  TapNoteScore tns = TNS_None;
  float offset = 0.0f;
};

struct MatchmakingOpponentStats {
  bool received = false;
  bool hasRates = false;  // pct/score/maxCombo/grade were actually sent
  bool hasHolds = false;  // held/letgo were actually sent
  bool hasMines = false;  // mines were actually sent
  float pct = 0.0f;
  unsigned int score = 0;
  int maxCombo = 0;
  std::string grade;
  int w1 = 0;
  int w2 = 0;
  int w3 = 0;
  int w4 = 0;
  int w5 = 0;
  int miss = 0;
  int held = 0;
  int letgo = 0;
  int mines = 0;
  bool failed = false;
};

class MatchmakingManager {
 public:
  MatchmakingManager();
  ~MatchmakingManager();

  enum SearchState {
    SearchState_Idle,
    SearchState_Connecting,
    SearchState_Queued,
    SearchState_Matched,
    SearchState_Playing,
  };

  void Update();
  void EnqueueMainThreadTask(std::function<void()> task);

  // Option row state.
  bool IsSearchEnabled() const { return m_bSearchRequested; }
  void SetSearchEnabled(bool b);

  // Screen flow.
  std::string FilterNextScreen(
      const std::string& currentScreenName, const std::string& toScreen);
  void CancelSearch();
  std::string GetResumeScreen() const { return m_sResumeScreen; }
  void BeginPlaying();

  SearchState GetState() const { return m_SearchState; }
  std::string GetStateString() const;
  float GetSearchElapsed() const;
  bool IsInMatch() const { return m_bInMatch; }
  bool IsNetworkPlayer(PlayerNumber pn) const;
  bool IsBotLocalPlayer(PlayerNumber pn) const;
  const std::string& GetOpponentName() const { return m_sOpponentName; }
  const std::string& GetOpponentCountry() const { return m_sOpponentCountry; }
  const std::string& GetOpponentIconPath() const { return m_sOpponentIconPath; }
  const MatchmakingOpponentStats& GetOpponentStats() const {
    return m_OpponentStats;
  }

  // Gameplay hooks.
  void NoteLocalJudgment(
      PlayerNumber pn, int row, int col, TapNoteScore tns, float offset);
  bool ConsumeNetworkJudgment(
      PlayerNumber pn, int col, int row, TapNoteScore& tnsOut,
      float& fOffsetOut);
  void GetDueNetworkEvents(
      int iRowNow, std::vector<NetworkJudgmentEvent>& out);
  void OnStageFinished();

  // Lua
  void PushSelf(lua_State* L);

 private:
  void Connect();
  void Disconnect();
  void SendJson(const Json::Value& root);
  void SendHello();
  void SendQueue();
  void SendResult();
  void HandleServerMessage(const Json::Value& root);
  void HandleMatched(const Json::Value& root);
  void ApplyOpponentMods(PlayerNumber pn, const std::string& mods);
  void HandleOpponentGone();
  void ProceedSolo();
  void ClearMatchState();
  void SetState(SearchState state);
  std::string GetServerUrl() const;
  std::string GetDisplayName() const;
  /* Icon sent in the hello message: the local profile's character card
   * image, base64-encoded ("" if none). */
  std::string GetLocalIconBase64() const;
  void SaveOpponentIcon(const std::string& base64Icon);
  void BroadcastStateChanged();

  void UpdateBotDriver();
  void BotWriteResultAndExit(bool bSuccess, const std::string& extra);

  static Preference<std::string> serverUrl;
  static Preference<std::string> playerName;

  ix::WebSocket m_WebSocket;
  ix::SocketTLSOptions m_TlsOptions;
  bool m_bWebSocketRunning = false;

  SearchState m_SearchState = SearchState_Idle;
  bool m_bSearchRequested = false;
  bool m_bSearchConsumed = false;
  std::string m_sResumeScreen;

  bool m_bInMatch = false;
  bool m_bNetworkPlayer[NUM_PLAYERS] = {false, false};
  PlayerNumber m_LocalPlayer = PLAYER_1;
  std::string m_sMatchId;
  std::string m_sRole;
  std::string m_sOpponentName;
  std::string m_sOpponentCountry;  // ISO code from the server, "" if unknown
  std::string m_sOpponentIconPath;
  MatchmakingOpponentStats m_OpponentStats;

  // Streamed remote judgments, keyed by (row, column): sorted by row so the
  // events due to be applied are always a prefix, and lookups are O(log n)
  // even when a lag spike buffers a large backlog.
  std::map<std::pair<int, int>, NetworkJudgmentEvent> m_RemoteEvents;
  bool m_bEventOverflowLogged = false;
  std::vector<NetworkJudgmentEvent> m_PendingLocalEvents;
  RageTimer m_LastJudgmentFlush;
  // Set when a search starts; backs GetSearchElapsed().
  RageTimer m_SearchStartTimer;
  // Last time we heard anything from the server (message or pong); main
  // thread only. Used to detect a silently dead connection mid-song.
  RageTimer m_LastInboundActivity;

  std::mutex m_MainThreadTaskMutex;
  std::queue<std::function<void()>> m_MainThreadTaskQueue;

  // Bot/E2E test mode (ITG_MM_BOT=1).
  bool m_bBotEnabled = false;
  bool m_bBotStarted = false;
  bool m_bBotDone = false;
  RageTimer m_BotTimer;
  // ITG_MM_2P_GUARD=1: verify search is inert with two local players joined.
  bool m_bBotTwoPlayerGuard = false;
  int m_iBotPhase = 0;  // 0 = play, 1 = returning to the wheel after a match
  RageTimer m_BotPhaseTimer;
  void BotWriteResultFile(const std::string& text, bool append);
};

extern MatchmakingManager* MATCHMAKING;

#endif
