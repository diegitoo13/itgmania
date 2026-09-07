#include "MatchmakingManager.h"

#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketMessageType.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "GameManager.h"
#include "GameState.h"
#include "Grade.h"
#include "LuaManager.h"
#include "MessageManager.h"
#include "NoteSkinManager.h"
#include "PlayerOptions.h"
#include "PlayerStageStats.h"
#include "PlayerState.h"
#include "Profile.h"
#include "ProfileManager.h"
#include "RageFile.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "Screen.h"
#include "ScreenManager.h"
#include "Song.h"
#include "SongManager.h"
#include "SpecialFiles.h"
#include "StatsManager.h"
#include "StdString.h"
#include "Steps.h"
#include "Style.h"
#include "Character.h"
#include "CharacterManager.h"
#include "json/json.h"
#include "mbedtls/base64.h"

MatchmakingManager* MATCHMAKING =
    nullptr;  // global and accessible from anywhere in our program

Preference<std::string> MatchmakingManager::serverUrl(
    "MatchmakingServerUrl", "wss://itg.kazza.jp/ws");
Preference<std::string> MatchmakingManager::playerName("MatchmakingName", "");

static const char* SearchStateNames[] = {
    "Idle", "Connecting", "Queued", "Matched", "Playing",
};

static constexpr int kMaxMainThreadTasksPerUpdate = 32;
static constexpr float kMaxMainThreadTaskSecondsPerUpdate = 0.001f;
static constexpr float kJudgmentFlushSeconds = 0.1f;
static constexpr float kBotStartupDelaySeconds = 2.0f;
static constexpr float kBotWatchdogSeconds = 600.0f;
// Cap on buffered unapplied remote judgments (a full chart is a few thousand
// notes); beyond this the remote stream is pathological and we drop events.
static constexpr size_t kMaxBufferedRemoteEvents = 4096;
// Seconds without any inbound traffic (messages or pongs) during a song
// before the connection is considered silently dead.
static constexpr float kNetworkStallSeconds = 20.0f;

MatchmakingManager::MatchmakingManager() {
  // Register with Lua.
  {
    Lua* L = LUA->Get();
    this->PushSelf(L);
    lua_setglobal(L, "MATCHMAKING");
    LUA->Release(L);
  }

  const char* botEnv = std::getenv("ITG_MM_BOT");
  m_bBotEnabled = botEnv != nullptr && botEnv[0] == '1';
  const char* guardEnv = std::getenv("ITG_MM_2P_GUARD");
  m_bBotTwoPlayerGuard = guardEnv != nullptr && guardEnv[0] == '1';
  if (m_bBotEnabled) {
    LOG->Trace("Matchmaking bot mode enabled (ITG_MM_BOT=1).");
    m_BotTimer.Touch();
  }

  // Migrate pre-feature installs: the old default pointed at localhost;
  // everyone should land on the public server unless they explicitly chose
  // something else.
  if (serverUrl.Get() == "ws://127.0.0.1:8765") {
    serverUrl.Set("wss://itg.kazza.jp/ws");
  }

  // Same TLS trust setup as NetworkManager, so wss:// servers with real
  // certificates verify correctly.
  RageFile f;
  if (f.Open(SpecialFiles::CA_BUNDLE_PATH)) {
    std::string data;
    f.Read(data);
    f.Close();
    m_TlsOptions.caFile = data;
  } else {
    LOG->Warn("Reading '%s' failed: %s", SpecialFiles::CA_BUNDLE_PATH.c_str(),
              f.GetError().c_str());
  }
}

MatchmakingManager::~MatchmakingManager() {
  // Unregister with Lua.
  LUA->UnsetGlobal("MATCHMAKING");

  Disconnect();
}

void MatchmakingManager::EnqueueMainThreadTask(std::function<void()> task) {
  std::lock_guard<std::mutex> lock(m_MainThreadTaskMutex);
  m_MainThreadTaskQueue.push(std::move(task));
}

std::string MatchmakingManager::GetStateString() const {
  return SearchStateNames[m_SearchState];
}

void MatchmakingManager::SetState(SearchState state) {
  if (m_SearchState == state) {
    return;
  }
  if (state == SearchState_Connecting) {
    m_SearchStartTimer.Touch();
  }
  m_SearchState = state;
  BroadcastStateChanged();
}

float MatchmakingManager::GetSearchElapsed() const {
  if (m_SearchState != SearchState_Connecting &&
      m_SearchState != SearchState_Queued) {
    return 0.0f;
  }
  return m_SearchStartTimer.Ago();
}

void MatchmakingManager::BroadcastStateChanged() {
  Message msg("MatchmakingStateChanged");
  msg.SetParam("state", GetStateString());
  MESSAGEMAN->Broadcast(msg);
}

bool MatchmakingManager::IsNetworkPlayer(PlayerNumber pn) const {
  if (pn < 0 || pn >= NUM_PLAYERS) {
    return false;
  }
  return m_bNetworkPlayer[pn];
}

bool MatchmakingManager::IsBotLocalPlayer(PlayerNumber pn) const {
  return m_bBotEnabled && pn == m_LocalPlayer;
}

void MatchmakingManager::SetSearchEnabled(bool b) {
  m_bSearchRequested = b;
  if (!b && !m_bInMatch) {
    m_bSearchConsumed = false;
  }
}

std::string MatchmakingManager::GetServerUrl() const {
  const char* env = std::getenv("ITG_MM_SERVER");
  if (env != nullptr && env[0] != '\0') {
    return env;
  }
  return serverUrl.Get();
}

std::string MatchmakingManager::GetDisplayName() const {
  const char* env = std::getenv("ITG_MM_NAME");
  if (env != nullptr && env[0] != '\0') {
    return env;
  }
  if (!playerName.Get().empty()) {
    return playerName.Get();
  }
  std::string name = PROFILEMAN->GetPlayerName(PLAYER_1);
  if (!name.empty()) {
    return name;
  }
  return "Player";
}

std::string MatchmakingManager::GetLocalIconPath() const {
  // Preferred source: the Simply Love avatar convention,
  // <profile dir>/avatar.{png,jpg,jpeg,bmp,gif} - one file then serves both
  // the theme's profile card and the online hello.
  const std::string sProfileDir =
      PROFILEMAN->GetProfileDir((ProfileSlot)m_LocalPlayer);
  if (!sProfileDir.empty()) {
    static const char* kAvatarExts[] = {"png", "jpg", "jpeg", "bmp", "gif"};
    for (const char* ext : kAvatarExts) {
      const std::string sPath = sProfileDir + "avatar." + ext;
      if (DoesFileExist(sPath)) {
        return sPath;
      }
    }
  }

  // Fallback: the profile's character card.
  Profile* pProfile = PROFILEMAN->GetProfile(m_LocalPlayer);
  if (pProfile == nullptr) {
    return "";
  }
  Character* pCharacter = pProfile->GetCharacter();
  if (pCharacter == nullptr) {
    return "";
  }
  return pCharacter->GetCardPath();
}

std::string MatchmakingManager::GetLocalIconBase64() const {
  auto encodeFile = [](const std::string& sPath) -> std::string {
    RageFile file;
    if (!file.Open(sPath)) {
      return "";
    }
    const int iSize = file.GetFileSize();
    // Keep the hello small: skip absurdly large images.
    if (iSize <= 0 || iSize > 512 * 1024) {
      return "";
    }
    std::string data(iSize, '\0');
    if (file.Read(data.data(), iSize) != iSize) {
      return "";
    }
    size_t iOutLen = 0;
    mbedtls_base64_encode(
        nullptr, 0, &iOutLen,
        reinterpret_cast<const unsigned char*>(data.data()), data.size());
    std::string out(iOutLen, '\0');
    size_t iWritten = 0;
    if (mbedtls_base64_encode(
            reinterpret_cast<unsigned char*>(out.data()), out.size(),
            &iWritten,
            reinterpret_cast<const unsigned char*>(data.data()),
            data.size()) != 0) {
      return "";
    }
    out.resize(iWritten);
    return out;
  };

  const std::string sPath = GetLocalIconPath();
  if (sPath.empty()) {
    return "";
  }
  return encodeFile(sPath);
}

void MatchmakingManager::Connect() {
  if (m_bWebSocketRunning) {
    return;
  }

  m_WebSocket.setUrl(GetServerUrl());
  m_WebSocket.setTLSOptions(m_TlsOptions);
  m_WebSocket.disableAutomaticReconnection();
  m_WebSocket.setPingInterval(5);
  m_WebSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open:
        EnqueueMainThreadTask([this]() {
          m_LastInboundActivity.Touch();
          LOG->Trace("Matchmaking: connected to %s", GetServerUrl().c_str());
          SendHello();
          if (m_SearchState == SearchState_Connecting) {
            SendQueue();
          }
        });
        break;
      case ix::WebSocketMessageType::Pong:
        EnqueueMainThreadTask([this]() { m_LastInboundActivity.Touch(); });
        break;
      case ix::WebSocketMessageType::Message: {
        const std::string& data = msg->str;
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(data, root)) {
          LOG->Warn("Matchmaking: failed to parse server message: %s",
                    data.c_str());
          break;
        }
        EnqueueMainThreadTask([this, root]() { HandleServerMessage(root); });
        break;
      }
      case ix::WebSocketMessageType::Close:
        EnqueueMainThreadTask([this]() {
          m_bWebSocketRunning = false;
          LOG->Trace("Matchmaking: connection closed.");
          if (m_SearchState == SearchState_Connecting ||
              m_SearchState == SearchState_Queued) {
            LOG->Trace("Matchmaking: lost connection while searching; "
                       "proceeding solo.");
            ProceedSolo();
          } else if (m_bInMatch) {
            HandleOpponentGone();
          }
        });
        break;
      case ix::WebSocketMessageType::Error: {
        const std::string reason = msg->errorInfo.reason;
        EnqueueMainThreadTask([this, reason]() {
          LOG->Warn("Matchmaking: connection error: %s", reason.c_str());
          m_bWebSocketRunning = false;
          if (m_SearchState == SearchState_Connecting ||
              m_SearchState == SearchState_Queued) {
            ProceedSolo();
          } else if (m_bInMatch) {
            HandleOpponentGone();
          }
        });
        break;
      }
      default:
        break;
    }
  });

  m_bWebSocketRunning = true;
  m_WebSocket.start();
}

void MatchmakingManager::Disconnect() {
  if (!m_bWebSocketRunning) {
    return;
  }
  m_WebSocket.disableAutomaticReconnection();
  m_WebSocket.stop();
  m_bWebSocketRunning = false;
}

void MatchmakingManager::SendJson(const Json::Value& root) {
  if (!m_bWebSocketRunning) {
    return;
  }
  Json::FastWriter writer;
  m_WebSocket.send(writer.write(root));
}

void MatchmakingManager::SendHello() {
  Json::Value root;
  root["cmd"] = "hello";
  root["name"] = GetDisplayName();
  std::string sIcon = GetLocalIconBase64();
  if (!sIcon.empty()) {
    root["icon"] = sIcon;
  }
  SendJson(root);
}

void MatchmakingManager::SendQueue() {
  Steps* pSteps = GAMESTATE->m_pCurSteps[m_LocalPlayer];
  Song* pSong = GAMESTATE->m_pCurSong;
  if (pSteps == nullptr || pSong == nullptr) {
    LOG->Warn("Matchmaking: no current song/steps; cannot queue.");
    ProceedSolo();
    return;
  }

  std::string key = pSteps->GetChartKey();
  std::string title = pSong->GetDisplayFullTitle();
  std::string artist = pSong->GetDisplayArtist();
  std::string pack = pSong->m_sGroupName;
  std::string difficulty = DifficultyToString(pSteps->GetDifficulty());
  int meter = pSteps->GetMeter();
  std::string stepsType = StepsTypeToString(pSteps->m_StepsType);
  if (key.empty()) {
    key = title + "|" + artist + "|" + pack + "|" + difficulty + "|" +
          std::to_string(meter) + "|" + stepsType;
  }

  Json::Value chart;
  chart["key"] = key;
  chart["title"] = title;
  chart["artist"] = artist;
  chart["pack"] = pack;
  chart["difficulty"] = difficulty;
  chart["meter"] = meter;
  chart["steps_type"] = stepsType;

  Json::Value root;
  root["cmd"] = "queue";
  root["chart"] = chart;
  // Our visual player options (speed, perspective, noteskin, ...) so the
  // opponent can show our notefield the way we see it. Option rows land in
  // different mod levels depending on the theme (Simply Love writes
  // Preferred, fallback-style rows write Stage), so concatenate both — later
  // tokens override earlier ones in FromString, matching engine precedence
  // (Stage wins over Preferred). Chart-altering mods are never applied on
  // the receiving side (see ApplyOpponentMods).
  PlayerState* ps = GAMESTATE->m_pPlayerState[m_LocalPlayer];
  root["mods"] = ps->m_PlayerOptions.GetPreferred().GetString() + ", " +
                 ps->m_PlayerOptions.GetStage().GetString();
  SendJson(root);
  LOG->Trace("Matchmaking: queued on chart '%s' (%s %s %d).", title.c_str(),
             difficulty.c_str(), stepsType.c_str(), meter);
}

void MatchmakingManager::SendResult() {
  const PlayerStageStats& pss =
      STATSMAN->m_CurStageStats.m_player[m_LocalPlayer];

  Json::Value stats;
  stats["pct"] = pss.GetPercentDancePoints();
  stats["score"] = pss.m_iScore;
  stats["max_combo"] = static_cast<int>(pss.m_iMaxCombo);
  stats["grade"] = GradeToString(pss.GetGrade());
  stats["w1"] = pss.m_iTapNoteScores[TNS_W1];
  stats["w2"] = pss.m_iTapNoteScores[TNS_W2];
  stats["w3"] = pss.m_iTapNoteScores[TNS_W3];
  stats["w4"] = pss.m_iTapNoteScores[TNS_W4];
  stats["w5"] = pss.m_iTapNoteScores[TNS_W5];
  stats["miss"] = pss.m_iTapNoteScores[TNS_Miss];
  stats["held"] = pss.m_iHoldNoteScores[HNS_Held];
  stats["letgo"] = pss.m_iHoldNoteScores[HNS_LetGo];
  stats["mines"] = pss.m_iTapNoteScores[TNS_HitMine];
  stats["failed"] = pss.m_bFailed;

  Json::Value root;
  root["cmd"] = "result";
  root["stats"] = stats;
  SendJson(root);
}

void MatchmakingManager::HandleServerMessage(const Json::Value& root) {
  m_LastInboundActivity.Touch();
  std::string cmd = root.get("cmd", "").asString();

  if (cmd == "welcome") {
    LOG->Trace("Matchmaking: server welcome (version %d).",
               root.get("version", 0).asInt());
  } else if (cmd == "queued") {
    LOG->Trace("Matchmaking: in queue at position %d.",
               root.get("position", 0).asInt());
    SetState(SearchState_Queued);
  } else if (cmd == "cancelled") {
    LOG->Trace("Matchmaking: queue cancelled by server.");
  } else if (cmd == "matched") {
    HandleMatched(root);
  } else if (cmd == "oj") {
    const Json::Value& events = root["events"];
    for (const Json::Value& ev : events) {
      NetworkJudgmentEvent event;
      event.row = ev.get("row", 0).asInt();
      event.col = ev.get("col", 0).asInt();
      event.tns = StringToTapNoteScore(ev.get("tns", "").asString());
      event.offset = ev.get("off", 0.0).asFloat();
      if (event.tns == TapNoteScore_Invalid) {
        continue;
      }
      if (m_RemoteEvents.size() >= kMaxBufferedRemoteEvents) {
        if (!m_bEventOverflowLogged) {
          LOG->Warn("Matchmaking: remote event backlog exceeded %d; dropping "
                    "new events.",
                    (int)kMaxBufferedRemoteEvents);
          m_bEventOverflowLogged = true;
        }
        continue;
      }
      m_RemoteEvents[std::make_pair(event.row, event.col)] = event;
    }
  } else if (cmd == "oresult") {
    const Json::Value& stats = root["stats"];
    m_OpponentStats.received = true;
    // Real game clients always send these; sparse clients (like the test
    // bot) omit them, in which case we keep our locally computed values.
    m_OpponentStats.hasRates = stats.isMember("pct");
    m_OpponentStats.hasHolds = stats.isMember("held");
    m_OpponentStats.hasMines = stats.isMember("mines");
    m_OpponentStats.pct = stats.get("pct", 0.0).asFloat();
    m_OpponentStats.score = stats.get("score", 0).asUInt();
    m_OpponentStats.maxCombo = stats.get("max_combo", 0).asInt();
    m_OpponentStats.grade = stats.get("grade", "").asString();
    m_OpponentStats.w1 = stats.get("w1", 0).asInt();
    m_OpponentStats.w2 = stats.get("w2", 0).asInt();
    m_OpponentStats.w3 = stats.get("w3", 0).asInt();
    m_OpponentStats.w4 = stats.get("w4", 0).asInt();
    m_OpponentStats.w5 = stats.get("w5", 0).asInt();
    m_OpponentStats.miss = stats.get("miss", 0).asInt();
    m_OpponentStats.held = stats.get("held", 0).asInt();
    m_OpponentStats.letgo = stats.get("letgo", 0).asInt();
    m_OpponentStats.mines = stats.get("mines", 0).asInt();
    m_OpponentStats.failed = stats.get("failed", false).asBool();

    Message msg("MatchmakingOpponentResult");
    msg.SetParam("name", root.get("name", "").asString());
    MESSAGEMAN->Broadcast(msg);
  } else if (cmd == "final") {
    LOG->Trace("Matchmaking: match %s complete.",
               root.get("match_id", "").asString().c_str());
  } else if (cmd == "odisconnect") {
    LOG->Trace("Matchmaking: opponent disconnected.");
    HandleOpponentGone();
  } else if (cmd == "error") {
    LOG->Warn("Matchmaking: server error: %s",
              root.get("message", "").asString().c_str());
  } else {
    LOG->Warn("Matchmaking: unknown server command '%s'.", cmd.c_str());
  }
}

void MatchmakingManager::SaveOpponentIcon(const std::string& base64Icon) {
  std::vector<uint8_t> png;
  size_t outLen = 0;
  mbedtls_base64_decode(
      nullptr, 0, &outLen,
      reinterpret_cast<const unsigned char*>(base64Icon.data()),
      base64Icon.size());
  png.resize(outLen);
  if (mbedtls_base64_decode(
          png.data(), png.size(), &outLen,
          reinterpret_cast<const unsigned char*>(base64Icon.data()),
          base64Icon.size()) != 0) {
    LOG->Warn("Matchmaking: failed to decode opponent icon.");
    return;
  }
  png.resize(outLen);

  RageFile f;
  if (!f.Open("Save/MatchmakingOpponent.png", RageFile::WRITE)) {
    LOG->Warn("Matchmaking: failed to save opponent icon: %s",
              f.GetError().c_str());
    return;
  }
  f.Write(png.data(), png.size());
  f.Close();
  m_sOpponentIconPath = "Save/MatchmakingOpponent.png";
}

void MatchmakingManager::HandleMatched(const Json::Value& root) {
  m_sMatchId = root.get("match_id", "").asString();
  m_sRole = root.get("role", "").asString();
  m_sOpponentName = root["opponent"].get("name", "Opponent").asString();
  m_sOpponentCountry = root["opponent"].get("country", "").asString();
  const Json::Value& icon = root["opponent"]["icon"];
  if (icon.isString() && !icon.asString().empty()) {
    SaveOpponentIcon(icon.asString());
  }

  LOG->Trace("Matchmaking: matched with '%s' (match %s, role %s).",
             m_sOpponentName.c_str(), m_sMatchId.c_str(), m_sRole.c_str());

  PlayerNumber opp = m_LocalPlayer == PLAYER_1 ? PLAYER_2 : PLAYER_1;

  if (!GAMESTATE->IsPlayerEnabled(opp) &&
      GAMESTATE->GetNumSidesJoined() == 1) {
    GAMESTATE->JoinPlayer(opp);
  }

  Steps* pSteps = GAMESTATE->m_pCurSteps[m_LocalPlayer];
  GAMESTATE->m_pCurSteps[opp].Set(pSteps);

  PlayerOptions poStage =
      GAMESTATE->m_pPlayerState[m_LocalPlayer]->m_PlayerOptions.GetStage();
  PlayerOptions poPreferred =
      GAMESTATE->m_pPlayerState[m_LocalPlayer]->m_PlayerOptions.GetPreferred();
  GAMESTATE->m_pPlayerState[opp]->m_PlayerOptions.Assign(
      ModsLevel_Stage, poStage);
  GAMESTATE->m_pPlayerState[opp]->m_PlayerOptions.Assign(
      ModsLevel_Preferred, poPreferred);

  // Both players must play the identical chart; strip chart-altering mods and
  // make sure the remote side never fails locally or trips autoplay.
  FOREACH_PlayerNumber(pn) {
    if (!GAMESTATE->IsPlayerEnabled(pn)) {
      continue;
    }
    PlayerState* ps = GAMESTATE->m_pPlayerState[pn];
    PlayerOptions stage = ps->m_PlayerOptions.GetStage();
    ZERO(stage.m_bTurns);
    ZERO(stage.m_bTransforms);
    if (pn == opp) {
      stage.m_FailType = FailType_Off;
      stage.m_fPlayerAutoPlay = 0.0f;
    }
    ps->m_PlayerOptions.Assign(ModsLevel_Stage, stage);

    PlayerOptions preferred = ps->m_PlayerOptions.GetPreferred();
    ZERO(preferred.m_bTurns);
    ZERO(preferred.m_bTransforms);
    if (pn == opp) {
      preferred.m_FailType = FailType_Off;
      preferred.m_fPlayerAutoPlay = 0.0f;
    }
    ps->m_PlayerOptions.Assign(ModsLevel_Preferred, preferred);
  }

  // Overlay the opponent's own visual setup (speed, perspective, noteskin,
  // ...) on their side, like a local P2 who set their own options.
  ApplyOpponentMods(opp, root["opponent"].get("mods", "").asString());

  Profile* pProfile = PROFILEMAN->GetProfile(opp);
  if (pProfile != nullptr) {
    pProfile->m_sDisplayName = m_sOpponentName;
  }

  m_bNetworkPlayer[opp] = true;
  m_bInMatch = true;

  Message msg("MatchmakingMatched");
  msg.SetParam("name", m_sOpponentName);
  msg.SetParam("icon", m_sOpponentIconPath);
  MESSAGEMAN->Broadcast(msg);

  SetState(SearchState_Matched);
}

void MatchmakingManager::ApplyOpponentMods(
    PlayerNumber pn, const std::string& mods) {
  if (mods.empty()) {
    return;
  }

  PlayerOptions po;
  po.FromString(mods);

  // Never let the network change the chart (rows/columns must stay 1:1 with
  // our copy), the life/fail behavior, or who is in control. Everything else
  // (scroll speed, perspective, noteskin, appearances, effects, ...) is
  // visual and safe to mirror.
  ZERO(po.m_bTurns);
  ZERO(po.m_bTransforms);
  po.m_twDisabledWindows.reset();
  po.m_FailType = FailType_Off;
  po.m_fPlayerAutoPlay = 0.0f;
  po.m_bMuteOnError = false;

  PlayerState* ps = GAMESTATE->m_pPlayerState[pn];
  const std::string sLocalSkin =
      ps->m_PlayerOptions.GetStage().m_sNoteSkin;
  if (po.m_sNoteSkin.empty() ||
      !NOTESKIN->DoesNoteSkinExist(po.m_sNoteSkin)) {
    po.m_sNoteSkin = sLocalSkin;
  }

  ps->m_PlayerOptions.Assign(ModsLevel_Stage, po);
  ps->m_PlayerOptions.Assign(ModsLevel_Preferred, po);
  LOG->Trace("Matchmaking: applied opponent mods '%s'.", mods.c_str());
}

void MatchmakingManager::HandleOpponentGone() {
  if (m_SearchState == SearchState_Playing) {
    Message msg("MatchmakingOpponentDisconnected");
    MESSAGEMAN->Broadcast(msg);

    // The opponent walked away from the cab: keep them as PC_NETWORK so
    // they never receive another input — every remaining note becomes a
    // miss as it ages out (the auto-miss path re-engages below once
    // IsInMatch() is false). Keep the network-player flag so teardown
    // still unjoins them at the wheel and their score is never saved.
    m_RemoteEvents.clear();
    // Nothing left to wait for: no result packet is coming, so don't stall
    // the evaluation screen for one.
    m_bInMatch = false;
    return;
  }

  // The opponent left before gameplay started; proceed solo.
  FOREACH_PlayerNumber(pn) {
    if (m_bNetworkPlayer[pn] && GAMESTATE->IsPlayerEnabled(pn)) {
      GAMESTATE->UnjoinPlayer(pn);
    }
  }
  ClearMatchState();
  SetState(SearchState_Idle);
}

void MatchmakingManager::ProceedSolo() {
  if (m_bWebSocketRunning) {
    Json::Value root;
    root["cmd"] = "cancel";
    SendJson(root);
  }
  FOREACH_PlayerNumber(pn) {
    if (m_bNetworkPlayer[pn] && GAMESTATE->IsPlayerEnabled(pn)) {
      GAMESTATE->UnjoinPlayer(pn);
    }
  }
  ClearMatchState();
  SetState(SearchState_Idle);
}

void MatchmakingManager::ClearMatchState() {
  m_bInMatch = false;
  FOREACH_PlayerNumber(pn) { m_bNetworkPlayer[pn] = false; }
  m_sMatchId.clear();
  m_sRole.clear();
  m_sOpponentName.clear();
  m_sOpponentCountry.clear();
  m_sOpponentIconPath.clear();
  m_OpponentStats = MatchmakingOpponentStats();
  m_RemoteEvents.clear();
  m_bEventOverflowLogged = false;
  m_PendingLocalEvents.clear();
}

std::string MatchmakingManager::FilterNextScreen(
    const std::string& currentScreenName, const std::string& toScreen) {
  bool bTargetsGameplay =
      toScreen == "ScreenStageInformation" ||
      toScreen.compare(0, 14, "ScreenGameplay") == 0;
  if (!m_bSearchRequested || m_bSearchConsumed || !bTargetsGameplay) {
    return toScreen;
  }
  m_LocalPlayer =
      GAMESTATE->IsPlayerEnabled(PLAYER_1) ? PLAYER_1 : PLAYER_2;
  if (GAMESTATE->m_pCurSong == nullptr ||
      GAMESTATE->m_pCurSteps[m_LocalPlayer] == nullptr) {
    return toScreen;
  }
  if (GAMESTATE->GetNumSidesJoined() != 1 || GAMESTATE->IsCourseMode() ||
      GAMESTATE->m_bDemonstrationOrJukebox) {
    return toScreen;
  }

  // The normal menu flow sets the play mode on the way to SelectMusic;
  // gameplay asserts on PlayMode_Invalid, so make sure it is valid here.
  if (GAMESTATE->m_PlayMode == PlayMode_Invalid) {
    GAMESTATE->m_PlayMode.Set(PLAY_MODE_REGULAR);
  }

  // Skip the stage-information beat for matches: go straight to gameplay
  // from the opponent-found sequence.
  m_sResumeScreen =
      toScreen == "ScreenStageInformation" ? "ScreenGameplay" : toScreen;
  m_bSearchConsumed = true;
  // If a previous search/match left the connection open, the Open handler
  // (which normally sends hello + queue) will never fire again — reuse the
  // live connection and queue directly on it.
  bool bWasConnected = m_bWebSocketRunning;
  SetState(SearchState_Connecting);
  Connect();
  if (bWasConnected) {
    SendQueue();
  }
  LOG->Trace("Matchmaking: redirecting %s -> ScreenMatchmaking (resume: %s).",
             currentScreenName.c_str(), m_sResumeScreen.c_str());
  return "ScreenMatchmaking";
}

void MatchmakingManager::CancelSearch() {
  LOG->Trace("Matchmaking: search cancelled.");
  ProceedSolo();
}

void MatchmakingManager::BeginPlaying() {
  if (!m_bInMatch) {
    return;
  }
  m_LastJudgmentFlush.Touch();
  SetState(SearchState_Playing);
}

void MatchmakingManager::NoteLocalJudgment(
    PlayerNumber pn, int row, int col, TapNoteScore tns, float offset) {
  if (!m_bInMatch || m_SearchState != SearchState_Playing ||
      pn != m_LocalPlayer || tns == TNS_None) {
    return;
  }
  PlayerController pc = GAMESTATE->m_pPlayerState[pn]->m_PlayerController;
  if (pc != PC_HUMAN && pc != PC_AUTOPLAY && pc != PC_CPU) {
    return;
  }

  NetworkJudgmentEvent event;
  event.row = row;
  event.col = col;
  event.tns = tns;
  event.offset = offset;
  m_PendingLocalEvents.push_back(event);
}

bool MatchmakingManager::ConsumeNetworkJudgment(
    PlayerNumber pn, int col, int row, TapNoteScore& tnsOut,
    float& fOffsetOut) {
  if (!IsNetworkPlayer(pn)) {
    return false;
  }
  auto it = m_RemoteEvents.find(std::make_pair(row, col));
  if (it == m_RemoteEvents.end()) {
    return false;
  }
  tnsOut = it->second.tns;
  fOffsetOut = it->second.offset;
  m_RemoteEvents.erase(it);
  return true;
}

void MatchmakingManager::GetDueNetworkEvents(
    int iRowNow, std::vector<NetworkJudgmentEvent>& out) {
  // Events are keyed by (row, col), so the due events form a prefix. They
  // are copied, not removed; the caller consumes them one by one via
  // ConsumeNetworkJudgment (directly for misses, through Player::Step for
  // hits), so events that couldn't be applied yet stay queued.
  out.clear();
  for (const auto& entry : m_RemoteEvents) {
    if (entry.first.first > iRowNow) {
      break;
    }
    out.push_back(entry.second);
  }
}

void MatchmakingManager::OnStageFinished() {
  if (!m_bInMatch) {
    return;
  }

  // Flush any remaining local judgments before sending the final result.
  if (!m_PendingLocalEvents.empty() && m_bWebSocketRunning) {
    Json::Value events(Json::arrayValue);
    for (const NetworkJudgmentEvent& event : m_PendingLocalEvents) {
      Json::Value ev;
      ev["row"] = event.row;
      ev["col"] = event.col;
      ev["tns"] = TapNoteScoreToString(event.tns);
      if (event.tns != TNS_Miss) {
        ev["off"] = event.offset;
      }
      events.append(ev);
    }
    Json::Value root;
    root["cmd"] = "j";
    root["events"] = events;
    SendJson(root);
    m_PendingLocalEvents.clear();
  }

  SendResult();

  // Both players finish the same song at nearly the same time, so the
  // opponent's result usually arrives within a few hundred milliseconds of
  // ours. Give their result packet a bounded moment to arrive so the
  // evaluation screen shows authoritative numbers instead of our local
  // approximation. An opponent who is just slow gets up to 6 seconds; one
  // who is gone already set m_bInMatch=false above and skips this.
  if (m_bInMatch && m_bWebSocketRunning && !m_OpponentStats.received) {
    RageTimer deadline;
    while (!m_OpponentStats.received && m_bInMatch && deadline.Ago() < 6.0f) {
      for (;;) {
        std::function<void()> task;
        {
          std::lock_guard<std::mutex> lock(m_MainThreadTaskMutex);
          if (m_MainThreadTaskQueue.empty()) {
            break;
          }
          task = std::move(m_MainThreadTaskQueue.front());
          m_MainThreadTaskQueue.pop();
        }
        task();
      }
      if (m_OpponentStats.received || !m_bInMatch) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  FOREACH_PlayerNumber(pn) {
    if (!m_bNetworkPlayer[pn] || !m_OpponentStats.received) {
      continue;
    }
    PlayerStageStats& pss = STATSMAN->m_CurStageStats.m_player[pn];
    const MatchmakingOpponentStats& st = m_OpponentStats;
    pss.m_iTapNoteScores[TNS_W1] = st.w1;
    pss.m_iTapNoteScores[TNS_W2] = st.w2;
    pss.m_iTapNoteScores[TNS_W3] = st.w3;
    pss.m_iTapNoteScores[TNS_W4] = st.w4;
    pss.m_iTapNoteScores[TNS_W5] = st.w5;
    pss.m_iTapNoteScores[TNS_Miss] = st.miss;
    if (st.hasMines) {
      pss.m_iTapNoteScores[TNS_HitMine] = st.mines;
    }
    if (st.hasHolds) {
      pss.m_iHoldNoteScores[HNS_Held] = st.held;
      pss.m_iHoldNoteScores[HNS_LetGo] = st.letgo;
    }
    if (st.hasRates) {
      pss.m_iMaxCombo = st.maxCombo;
      pss.m_iScore = st.score;
      pss.m_iActualDancePoints =
          static_cast<int>(std::lround(st.pct * pss.m_iPossibleDancePoints));
    }
    pss.m_bFailed = st.failed;
    pss.m_bDisqualified = false;

    pss.m_HighScore.SetName(m_sOpponentName);
    if (st.hasRates) {
      pss.m_HighScore.SetScore(st.score);
      pss.m_HighScore.SetPercentDP(st.pct);
      pss.m_HighScore.SetMaxCombo(st.maxCombo);
    }
    pss.m_HighScore.SetDisqualified(false);
    pss.m_HighScore.SetTapNoteScore(TNS_W1, st.w1);
    pss.m_HighScore.SetTapNoteScore(TNS_W2, st.w2);
    pss.m_HighScore.SetTapNoteScore(TNS_W3, st.w3);
    pss.m_HighScore.SetTapNoteScore(TNS_W4, st.w4);
    pss.m_HighScore.SetTapNoteScore(TNS_W5, st.w5);
    pss.m_HighScore.SetTapNoteScore(TNS_Miss, st.miss);
    if (st.hasMines) {
      pss.m_HighScore.SetTapNoteScore(TNS_HitMine, st.mines);
    }
    if (st.hasHolds) {
      pss.m_HighScore.SetHoldNoteScore(HNS_Held, st.held);
      pss.m_HighScore.SetHoldNoteScore(HNS_LetGo, st.letgo);
    }
    if (!st.grade.empty()) {
      Grade g = StringToGrade(st.grade);
      if (g != Grade_Invalid) {
        pss.m_HighScore.SetGrade(g);
      }
    }
  }
}

void MatchmakingManager::Update() {
  RageTimer timer;
  int tasksProcessed = 0;

  while (tasksProcessed < kMaxMainThreadTasksPerUpdate) {
    std::function<void()> task;
    {
      std::lock_guard<std::mutex> lock(m_MainThreadTaskMutex);
      if (m_MainThreadTaskQueue.empty()) {
        break;
      }

      task = std::move(m_MainThreadTaskQueue.front());
      m_MainThreadTaskQueue.pop();
    }

    task();
    ++tasksProcessed;

    if (timer.Ago() >= kMaxMainThreadTaskSecondsPerUpdate) {
      break;
    }
  }

  // Stream out local judgments in batches while playing.
  if (m_SearchState == SearchState_Playing && m_bInMatch &&
      !m_PendingLocalEvents.empty() &&
      m_LastJudgmentFlush.Ago() >= kJudgmentFlushSeconds) {
    Json::Value events(Json::arrayValue);
    for (const NetworkJudgmentEvent& event : m_PendingLocalEvents) {
      Json::Value ev;
      ev["row"] = event.row;
      ev["col"] = event.col;
      ev["tns"] = TapNoteScoreToString(event.tns);
      if (event.tns != TNS_Miss) {
        ev["off"] = event.offset;
      }
      events.append(ev);
    }
    Json::Value root;
    root["cmd"] = "j";
    root["events"] = events;
    SendJson(root);
    m_PendingLocalEvents.clear();
    m_LastJudgmentFlush.Touch();
  }

  // osu!-style liveness: if the connection goes silent mid-song (no messages
  // or pongs), treat the opponent as gone instead of freezing P2 forever.
  if (m_SearchState == SearchState_Playing && m_bInMatch &&
      m_bWebSocketRunning &&
      m_LastInboundActivity.Ago() > kNetworkStallSeconds) {
    LOG->Warn("Matchmaking: no traffic for %.0fs mid-song; treating the "
              "opponent as gone.",
              kNetworkStallSeconds);
    Json::Value root;
    root["cmd"] = "leave";
    SendJson(root);
    Disconnect();
    HandleOpponentGone();
  }

  // The flow has left gameplay for good; tear down any match state.
  if (SCREENMAN != nullptr) {
    Screen* pScreen = SCREENMAN->GetTopScreen();
    if (pScreen != nullptr) {
      const std::string& name = pScreen->GetName();
      bool bBackToMusicSelect =
          name.find("SelectMusic") != std::string::npos ||
          name.find("SelectSong") != std::string::npos;
      bool bBackToTitle = name.find("ScreenTitleMenu") != std::string::npos ||
                          name.find("ScreenAttract") != std::string::npos;
      if ((bBackToMusicSelect || bBackToTitle) &&
          (m_bInMatch || m_bSearchConsumed || m_bSearchRequested)) {
        if (m_bInMatch && m_bWebSocketRunning) {
          Json::Value root;
          root["cmd"] = "leave";
          SendJson(root);
        }
        FOREACH_PlayerNumber(pn) {
          if (m_bNetworkPlayer[pn] && GAMESTATE->IsPlayerEnabled(pn)) {
            GAMESTATE->UnjoinPlayer(pn);
          }
        }
        ClearMatchState();
        m_bSearchRequested = false;
        m_bSearchConsumed = false;
        m_sResumeScreen.clear();
        SetState(SearchState_Idle);
      }
    }
  }

  if (m_bBotEnabled || m_bBotTwoPlayerGuard) {
    UpdateBotDriver();
  }
}

void MatchmakingManager::UpdateBotDriver() {
  if (m_bBotDone) {
    return;
  }

  if (!m_bBotStarted) {
    if (m_BotTimer.Ago() < kBotStartupDelaySeconds) {
      return;
    }
    if (SONGMAN == nullptr || SCREENMAN == nullptr ||
        SCREENMAN->GetTopScreen() == nullptr || GAMESTATE == nullptr) {
      return;
    }

    Song* pSong = nullptr;
    Steps* pSteps = nullptr;
    for (Song* song : SONGMAN->GetAllSongs()) {
      for (Steps* steps : song->GetAllSteps()) {
        if (steps->m_StepsType == StepsType_dance_single) {
          pSong = song;
          pSteps = steps;
          break;
        }
      }
      if (pSteps != nullptr) {
        break;
      }
    }
    if (pSteps == nullptr) {
      return;
    }

    // Two-local-players guard: with both sides joined, requesting a search
    // must be inert (no redirect into ScreenMatchmaking, state stays Idle).
    if (m_bBotTwoPlayerGuard) {
      GAMESTATE->JoinPlayer(PLAYER_1);
      GAMESTATE->JoinPlayer(PLAYER_2);
      GAMESTATE->m_pCurSong.Set(pSong);
      GAMESTATE->m_pCurSteps[PLAYER_1].Set(pSteps);
      GAMESTATE->m_pCurSteps[PLAYER_2].Set(pSteps);
      SetSearchEnabled(true);
      std::string filtered =
          FilterNextScreen("ScreenPlayerOptions", "ScreenStageInformation");
      bool ok = filtered == "ScreenStageInformation" &&
                GAMESTATE->GetNumSidesJoined() == 2 &&
                m_SearchState == SearchState_Idle;
      BotWriteResultFile(
          ssprintf("MATCHMAKING_2P_GUARD_%s filtered=%s joined=%d state=%s\n",
                   ok ? "OK" : "FAIL", filtered.c_str(),
                   GAMESTATE->GetNumSidesJoined(), GetStateString().c_str()),
          false);
      std::quick_exit(ok ? 0 : 1);
    }

    LOG->Trace("Matchmaking bot: selected '%s'.",
               pSong->GetDisplayFullTitle().c_str());

    if (GAMESTATE->GetNumSidesJoined() == 0) {
      GAMESTATE->JoinPlayer(PLAYER_1);
    }
    if (GAMESTATE->m_pCurGame != nullptr) {
      const Style* pStyle = GAMEMAN->GetFirstCompatibleStyle(
          GAMESTATE->m_pCurGame, 1, StepsType_dance_single);
      if (pStyle != nullptr) {
        GAMESTATE->SetCurrentStyle(pStyle, PLAYER_1);
      }
    }
    GAMESTATE->m_pCurSong.Set(pSong);
    GAMESTATE->m_pCurSteps[PLAYER_1].Set(pSteps);

    PlayerOptions poStage =
        GAMESTATE->m_pPlayerState[PLAYER_1]->m_PlayerOptions.GetStage();
    poStage.m_FailType = FailType_Off;
    GAMESTATE->m_pPlayerState[PLAYER_1]->m_PlayerOptions.Assign(
        ModsLevel_Stage, poStage);
    PlayerOptions poPreferred =
        GAMESTATE->m_pPlayerState[PLAYER_1]->m_PlayerOptions.GetPreferred();
    poPreferred.m_FailType = FailType_Off;
    GAMESTATE->m_pPlayerState[PLAYER_1]->m_PlayerOptions.Assign(
        ModsLevel_Preferred, poPreferred);

    // Same PlayMode fixup as FilterNextScreen: this path bypasses the menu
    // flow that normally sets it.
    if (GAMESTATE->m_PlayMode == PlayMode_Invalid) {
      GAMESTATE->m_PlayMode.Set(PLAY_MODE_REGULAR);
    }

    SetSearchEnabled(true);
    m_sResumeScreen = "ScreenGameplay";
    m_bSearchConsumed = true;
    m_LocalPlayer = PLAYER_1;
    SetState(SearchState_Connecting);
    Connect();
    SCREENMAN->SetNewScreen("ScreenMatchmaking");

    m_bBotStarted = true;
    m_BotTimer.Touch();
    return;
  }

  Screen* pScreen = SCREENMAN->GetTopScreen();
  std::string screenName = pScreen != nullptr ? pScreen->GetName() : "";

  // Phase 1: the match is over and we sent the player back to the music
  // wheel. The Update() screen watch must have torn the match down: the
  // network player unjoined, search state reset (so the next song starts
  // as a normal single-player game until the row is enabled again).
  if (m_iBotPhase == 1) {
    bool bAtWheel = screenName.find("SelectMusic") != std::string::npos ||
                    screenName.find("SelectSong") != std::string::npos;
    bool bTornDown = GAMESTATE->GetNumSidesJoined() == 1 && !m_bInMatch &&
                     !m_bSearchRequested && !m_bSearchConsumed &&
                     m_SearchState == SearchState_Idle;
    if (bAtWheel && bTornDown) {
      BotWriteResultFile("MATCHMAKING_TEARDOWN_OK\n", true);
      std::quick_exit(0);
    }
    if (m_BotPhaseTimer.Ago() > 15.0f) {
      BotWriteResultFile(
          ssprintf("MATCHMAKING_TEARDOWN_FAIL screen=%s joined=%d inmatch=%d "
                   "requested=%d consumed=%d state=%s\n",
                   screenName.c_str(), GAMESTATE->GetNumSidesJoined(),
                   m_bInMatch ? 1 : 0, m_bSearchRequested ? 1 : 0,
                   m_bSearchConsumed ? 1 : 0, GetStateString().c_str()),
          true);
      std::quick_exit(1);
    }
    return;
  }

  if (screenName.compare(0, 16, "ScreenEvaluation") == 0 &&
      !STATSMAN->m_vPlayedStageStats.empty()) {
    const StageStats& ss = STATSMAN->m_vPlayedStageStats.back();
    std::string text;
    FOREACH_PlayerNumber(pn) {
      if (!GAMESTATE->IsPlayerEnabled(pn)) {
        continue;
      }
      const PlayerStageStats& pss = ss.m_player[pn];
      text += ssprintf(
          "P%d name=%s pct=%.4f score=%u max_combo=%u grade=%s "
          "w1=%d w2=%d w3=%d w4=%d w5=%d miss=%d mines=%d held=%d letgo=%d "
          "failed=%d\n",
          pn + 1, GAMESTATE->GetPlayerDisplayName(pn).c_str(),
          pss.GetPercentDancePoints(), pss.m_iScore, pss.m_iMaxCombo,
          GradeToString(pss.GetGrade()).c_str(), pss.m_iTapNoteScores[TNS_W1],
          pss.m_iTapNoteScores[TNS_W2], pss.m_iTapNoteScores[TNS_W3],
          pss.m_iTapNoteScores[TNS_W4], pss.m_iTapNoteScores[TNS_W5],
          pss.m_iTapNoteScores[TNS_Miss], pss.m_iTapNoteScores[TNS_HitMine],
          pss.m_iHoldNoteScores[HNS_Held], pss.m_iHoldNoteScores[HNS_LetGo],
          pss.m_bFailed ? 1 : 0);
    }
    const MatchmakingOpponentStats& st = m_OpponentStats;
    text += ssprintf(
        "OPP name=%s w1=%d w2=%d w3=%d w4=%d w5=%d miss=%d pct=%.4f "
        "country=%s\n",
        m_sOpponentName.c_str(), st.w1, st.w2, st.w3, st.w4, st.w5, st.miss,
        st.pct, m_sOpponentCountry.c_str());
    FOREACH_PlayerNumber(pn) {
      if (m_bNetworkPlayer[pn]) {
        text += ssprintf(
            "P%dMODS mods=%s\n", pn + 1,
            GAMESTATE->m_pPlayerState[pn]
                ->m_PlayerOptions.GetStage()
                .GetString()
                .c_str());
      }
    }
    text += "MATCHMAKING_E2E_DONE\n";
    BotWriteResultFile(text, false);

    // Simulate the player hitting Enter on the results screen to go back
    // to the music wheel, then verify match teardown in phase 1.
    SCREENMAN->SetNewScreen("ScreenSelectMusic");
    m_iBotPhase = 1;
    m_BotPhaseTimer.Touch();
    return;
  }

  if (m_BotTimer.Ago() > kBotWatchdogSeconds) {
    BotWriteResultFile(
        ssprintf("MATCHMAKING_E2E_FAIL state=%s screen=%s\n",
                 GetStateString().c_str(), screenName.c_str()),
        false);
    std::quick_exit(1);
  }
}

void MatchmakingManager::BotWriteResultFile(
    const std::string& text, bool append) {
  const char* resultPath = std::getenv("ITG_MM_RESULT");
  std::string path =
      resultPath != nullptr && resultPath[0] != '\0' ? resultPath
                                                     : "/tmp/itg_mm_result.txt";

  std::ofstream file(
      path, std::ios::out | (append ? std::ios::app : std::ios::trunc));
  file << text;
  file.flush();
  file.close();
}

void MatchmakingManager::BotWriteResultAndExit(
    bool bSuccess, const std::string& extra) {
  m_bBotDone = true;
  BotWriteResultFile(
      bSuccess ? extra + "MATCHMAKING_E2E_DONE\n"
               : "MATCHMAKING_E2E_FAIL " + extra + "\n",
      false);
  std::quick_exit(bSuccess ? 0 : 1);
}

// lua start
#include "LuaBinding.h"

/** @brief Allow Lua to have access to the MatchmakingManager. */
class LunaMatchmakingManager : public Luna<MatchmakingManager> {
 public:
  static int IsSearchEnabled(T* p, lua_State* L) {
    lua_pushboolean(L, p->IsSearchEnabled());
    return 1;
  }

  static int SetSearchEnabled(T* p, lua_State* L) {
    p->SetSearchEnabled(lua_toboolean(L, 1));
    return 0;
  }

  static int GetState(T* p, lua_State* L) {
    lua_pushstring(L, p->GetStateString().c_str());
    return 1;
  }

  static int GetSearchElapsed(T* p, lua_State* L) {
    lua_pushnumber(L, p->GetSearchElapsed());
    return 1;
  }

  static int IsInMatch(T* p, lua_State* L) {
    lua_pushboolean(L, p->IsInMatch());
    return 1;
  }

  static int IsNetworkPlayer(T* p, lua_State* L) {
    PlayerNumber pn = Enum::Check<PlayerNumber>(L, 1);
    lua_pushboolean(L, p->IsNetworkPlayer(pn));
    return 1;
  }

  static int GetOpponentName(T* p, lua_State* L) {
    lua_pushstring(L, p->GetOpponentName().c_str());
    return 1;
  }

  static int GetOpponentCountry(T* p, lua_State* L) {
    lua_pushstring(L, p->GetOpponentCountry().c_str());
    return 1;
  }

  static int GetOpponentIconPath(T* p, lua_State* L) {
    lua_pushstring(L, p->GetOpponentIconPath().c_str());
    return 1;
  }

  static int GetLocalIconPath(T* p, lua_State* L) {
    lua_pushstring(L, p->GetLocalIconPath().c_str());
    return 1;
  }

  static int GetOpponentStats(T* p, lua_State* L) {
    const MatchmakingOpponentStats& st = p->GetOpponentStats();
    lua_newtable(L);
    lua_pushboolean(L, st.received);
    lua_setfield(L, -2, "received");
    lua_pushnumber(L, st.pct);
    lua_setfield(L, -2, "pct");
    lua_pushinteger(L, st.score);
    lua_setfield(L, -2, "score");
    lua_pushinteger(L, st.maxCombo);
    lua_setfield(L, -2, "max_combo");
    lua_pushstring(L, st.grade.c_str());
    lua_setfield(L, -2, "grade");
    lua_pushinteger(L, st.w1);
    lua_setfield(L, -2, "w1");
    lua_pushinteger(L, st.w2);
    lua_setfield(L, -2, "w2");
    lua_pushinteger(L, st.w3);
    lua_setfield(L, -2, "w3");
    lua_pushinteger(L, st.w4);
    lua_setfield(L, -2, "w4");
    lua_pushinteger(L, st.w5);
    lua_setfield(L, -2, "w5");
    lua_pushinteger(L, st.miss);
    lua_setfield(L, -2, "miss");
    lua_pushinteger(L, st.held);
    lua_setfield(L, -2, "held");
    lua_pushinteger(L, st.letgo);
    lua_setfield(L, -2, "letgo");
    lua_pushinteger(L, st.mines);
    lua_setfield(L, -2, "mines");
    lua_pushboolean(L, st.failed);
    lua_setfield(L, -2, "failed");
    return 1;
  }

  static int CancelSearch(T* p, lua_State* L) {
    p->CancelSearch();
    return 0;
  }

  static int GetResumeScreen(T* p, lua_State* L) {
    lua_pushstring(L, p->GetResumeScreen().c_str());
    return 1;
  }

  LunaMatchmakingManager() {
    ADD_METHOD(IsSearchEnabled);
    ADD_METHOD(SetSearchEnabled);
    ADD_METHOD(GetState);
    ADD_METHOD(GetSearchElapsed);
    ADD_METHOD(IsInMatch);
    ADD_METHOD(IsNetworkPlayer);
    ADD_METHOD(GetOpponentName);
    ADD_METHOD(GetOpponentCountry);
    ADD_METHOD(GetOpponentIconPath);
  ADD_METHOD(GetLocalIconPath);
    ADD_METHOD(GetOpponentStats);
    ADD_METHOD(CancelSearch);
    ADD_METHOD(GetResumeScreen);
  }
};

LUA_REGISTER_CLASS(MatchmakingManager)
// lua end
