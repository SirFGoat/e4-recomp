// e4 - worldwide leaderboards (Supabase).
//
// Registers a rex StatsProvider: the game's XSessionWriteStats calls are queued and
// posted to the Supabase function submit_score (keeps each player's best per board;
// weekly boards reset every ISO week), and its leaderboard reads (XUserCreateStats-
// EnumeratorBy*) are answered from get_board. Scores that can't be sent (offline) are
// kept in leaderboard_pending.jsonl next to e4 and retried at the next start.
//
// Identity comes from the launcher: user_name (gamertag), user_xuid (random player ID)
// and e4_player_token (secret key; the server only accepts a player ID's scores with
// the key it was first used with).
//
// The in-game HI-SCORE (top right) normally shows the #1 of the game's local top-10
// table, whose default is 1,000,000,000,000. With e4_hud_online_best it shows the
// player's own online best for the board being played instead.

#include "generated/default/e4_init.h"

#include "e4_leaderboard.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cctype>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/sync_record.h>
#include <rex/system/xam/stats_provider.h>

#include "e4_http.h"
#include "third_party/nlohmann/json.hpp"

REXCVAR_DEFINE_BOOL(e4_leaderboards, true, "E4", "Send scores to / read the online leaderboards");
REXCVAR_DEFINE_STRING(e4_leaderboard_url, "https://pjsinkjifjpenmcsbxew.supabase.co", "E4",
                      "Leaderboard server (Supabase project URL)");
REXCVAR_DEFINE_STRING(e4_leaderboard_key, "sb_publishable_-haMIT7jYBFqeABCydUdDg_Wq5RvmYM", "E4",
                      "Leaderboard server public API key");
REXCVAR_DEFINE_STRING(e4_player_token, "", "E4",
                      "Secret player key from the launcher (proves the player ID is yours)");
REXCVAR_DEFINE_BOOL(e4_hud_online_best, true, "E4",
                    "In-game HI-SCORE shows your online best for the board instead of the local "
                    "top-10 (default 1,000,000,000,000)");
REXCVAR_DECLARE(std::string, user_xuid);
REXCVAR_DEFINE_DOUBLE(e4_beat_offset_beats, 0.5, "E4",
                      "Run the beat clock (beat bar, lamps, vibration, beat-timed sounds) this many "
                      "beats behind the music. 0.5 puts the beat on the music's off-beat, where it "
                      "is felt; being a whole eighth note, beat-timed sounds stay on the music's "
                      "grid at every tempo (0 = original timing)");
REXCVAR_DEFINE_INT32(e4_beat_offset_ms, 0, "E4",
                     "Extra beat clock delay in ms on top of e4_beat_offset_beats (fine-tuning "
                     "for a particular TV / speaker setup)");

namespace e4::leaderboard {
namespace {

using json = nlohmann::json;
namespace xam = rex::system::xam;

// The game's stats views (from its XLAST): column attribute id -> property id.
// Attribute 65534 is the rating (sort) column.
struct Board {
  uint32_t view_id;
  const char* name;
  std::vector<std::pair<uint16_t, uint32_t>> columns;
};
const std::vector<Board> kBoards = {
    {1, "WeeklyUnlimited_Stage1", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {2, "WeeklyUnlimited_Stage2", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {3, "NormalGame_VeryEasy", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x10000004}, {3, 0x10000003}}},
    {4, "NormalGame_Easy", {{65534, 0x20000001}, {2, 0x20000001}, {3, 0x10000004}, {1, 0x10000003}}},
    {5, "NormalGame_Normal", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x10000004}, {3, 0x10000003}}},
    {6, "NormalGame_Hard", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x10000004}, {3, 0x10000003}}},
    {7, "NormalGame_VeryHard", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x10000004}, {3, 0x10000003}}},
    {8, "UnlimitedGame_Stage1", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {9, "UnlimitedGame_Stage2", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {10, "UnlimitedGame_Stage3", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {11, "UnlimitedGame_Stage4", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {12, "UnlimitedGame_Stage5", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {13, "Rating_NormalGame", {{1, 0x30000006}, {65534, 0x20000007}}},
    {14, "Rating_UnlimitedGame", {{65534, 0x20000007}, {2, 0x30000006}, {1, 0x20000007}}},
    {15, "ShootingGame", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x10000004}}},
    {16, "TimeLimitGame_Stage1", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {17, "TimeLimitGame_Stage2", {{1, 0x20000001}, {65534, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {18, "TimeLimitGame_Stage3", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {19, "TimeLimitGame_Stage4", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {20, "H4_Ranked_VersusGame", {{65534, 0x2000000A}, {2, 0x2000000B}, {1, 0x2000000A}}},
    {21, "H4_Player_VersusGame", {{2, 0x2000000B}, {65534, 0x2000000A}, {1, 0x2000000A}}},
    {22, "WeeklyUnlimited_Stage3", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {23, "WeeklyUnlimited_Stage4", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {24, "WeeklyTimelimit_Stage1", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {25, "WeeklyTimelimit_Stage2", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {26, "WeeklyTimelimit_Stage3", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {27, "WeeklyTimelimit_Stage4", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x30000005}, {3, 0x10000003}}},
    {28, "WeeklyR4", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x10000004}}},
    {29, "WeeklyH4Ranked", {{65534, 0x2000000A}, {1, 0x2000000B}, {2, 0x2000000A}}},
    {30, "WeeklyRating", {{65534, 0x20000007}, {1, 0x30000006}, {2, 0x20000007}}},
    {31, "S4_WizUrMusik", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x3000000C}, {3, 0x2000000D}, {4, 0x2000000E}, {5, 0x2000000F}, {6, 0x20000010}, {7, 0x20000011}, {8, 0x20000012}, {9, 0x20000013}, {10, 0x20000014}, {11, 0x20000015}, {12, 0x20000016}, {13, 0x20000017}, {14, 0x20000018}, {15, 0x20000019}, {16, 0x2000001A}, {17, 0x2000001B}, {18, 0x2000001C}, {19, 0x2000001D}, {20, 0x2000001E}, {21, 0x2000001F}, {22, 0x20000020}}},
    {32, "WeeklyS4", {{65534, 0x20000001}, {1, 0x20000001}, {2, 0x3000000C}, {3, 0x2000000D}, {4, 0x2000000E}, {5, 0x2000000F}, {6, 0x20000010}, {7, 0x20000011}, {8, 0x20000012}, {9, 0x20000013}, {10, 0x20000014}, {11, 0x20000015}, {12, 0x20000016}, {13, 0x20000017}, {14, 0x20000018}, {15, 0x20000019}, {16, 0x2000001A}, {17, 0x2000001B}, {18, 0x2000001C}, {19, 0x2000001D}, {20, 0x2000001E}, {21, 0x2000001F}, {22, 0x20000020}}},
};

const Board* FindBoard(uint32_t view_id) {
  for (const auto& b : kBoards) {
    if (b.view_id == view_id) return &b;
  }
  return nullptr;
}

// Property ids encode their XUSER_DATA type in the top nibble.
xam::StatsDataType TypeOfProperty(uint32_t property_id) {
  switch (property_id >> 28) {
    case 1:
      return xam::StatsDataType::kInt32;
    case 2:
      return xam::StatsDataType::kInt64;
    case 3:
      return xam::StatsDataType::kDouble;
    default:
      return xam::StatsDataType::kNone;
  }
}

std::string Hex8(uint32_t v) {
  char b[9];
  std::snprintf(b, sizeof(b), "%08X", v);
  return b;
}
std::string Hex16(uint64_t v) {
  char b[17];
  std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)v);
  return b;
}

constexpr uint64_t kDefaultXuid = 0xB13EBABEBABEBABEull;  // no launcher identity
constexpr int kTimeoutMs = 6000;

class SupabaseStats final : public xam::StatsProvider {
 public:
  SupabaseStats() {
    url_ = REXCVAR_GET(e4_leaderboard_url);
    key_ = REXCVAR_GET(e4_leaderboard_key);
    token_ = REXCVAR_GET(e4_player_token);
    std::error_code ec;
    pending_path_ = std::filesystem::current_path(ec) / "leaderboard_pending.jsonl";
    LoadPending();
    worker_ = std::thread([this] { Run(); });
    worker_.detach();  // the process hard-exits; never join at static teardown
  }

  void WriteStats(uint64_t xuid, const std::string& gamertag, uint32_t view_id,
                  const std::vector<xam::StatsProperty>& properties) override {
    if (xuid == kDefaultXuid || token_.size() < 32 || gamertag.empty()) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        REXLOG_WARN(
            "e4 leaderboard: not submitting - no player identity (start the game from the "
            "launcher)");
      }
      return;
    }
    if (!FindBoard(view_id)) return;
    json columns = json::object();
    for (const auto& p : properties) {
      if (p.value.type == xam::StatsDataType::kDouble) {
        columns[Hex8(p.property_id)] = p.value.dbl;
      } else {
        columns[Hex8(p.property_id)] = p.value.i64;
      }
    }
    json body = {{"p_board", view_id},
                 {"p_xuid", Hex16(xuid)},
                 {"p_token", token_},
                 {"p_gamertag", gamertag},
                 {"p_columns", columns}};
    REXLOG_INFO("e4 leaderboard: queued board {} for {}", view_id, gamertag);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(body.dump());
    }
    cv_.notify_all();
  }

  bool ReadStats(xam::StatsReadMode mode, uint64_t pivot, uint32_t num_rows,
                 const xam::StatsSpec& spec, xam::StatsViewResult& out) override {
    out.view_id = spec.view_id;
    const Board* board = FindBoard(spec.view_id);
    if (!board) return false;
    // A score submitted a moment ago should show up: let the queue drain first.
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::seconds(4),
                   [this] { return queue_.empty() && !sending_; });
    }
    std::string pivot_text = mode == xam::StatsReadMode::kByXuid  ? Hex16(pivot)
                             : mode == xam::StatsReadMode::kByRank ? std::to_string(pivot)
                                                                   : std::string("1");
    json body = {{"p_board", spec.view_id},
                 {"p_mode", mode == xam::StatsReadMode::kByXuid ? 0 : 1},
                 {"p_pivot", pivot_text},
                 {"p_rows", num_rows}};
    auto r = Call("get_board", body.dump());
    if (r.status != 200) {
      REXLOG_WARN("e4 leaderboard: read of board {} failed: {} {}", spec.view_id, r.status,
                  r.status ? r.body.substr(0, 200) : r.error);
      return false;
    }
    json rows = json::parse(r.body, nullptr, false);
    if (!rows.is_array()) return false;
    for (const auto& j : rows) {
      if (!j.is_object()) continue;
      xam::StatsRow row;
      row.xuid = std::strtoull(j.value("xuid", std::string("0")).c_str(), nullptr, 16);
      row.rank = j.value("rank", 0u);
      row.rating = j.value("rating", int64_t(0));
      row.gamertag = j.value("gamertag", std::string());
      out.total_rows = j.value("total", 0u);
      json cols = j.contains("columns") && j["columns"].is_object() ? j["columns"] : json::object();
      for (uint16_t column_id : spec.column_ids) {
        uint32_t property = 0;
        for (const auto& [attr, prop] : board->columns) {
          if (attr == column_id) property = prop;
        }
        if (!property) continue;
        auto it = cols.find(Hex8(property));
        if (it == cols.end() || !it->is_number()) continue;
        xam::StatsColumn c;
        c.column_id = column_id;
        c.value.type = TypeOfProperty(property);
        if (c.value.type == xam::StatsDataType::kDouble) {
          c.value.dbl = it->get<double>();
        } else {
          c.value.i64 = it->is_number_float() ? int64_t(it->get<double>()) : it->get<int64_t>();
        }
        row.columns.push_back(c);
      }
      out.rows.push_back(std::move(row));
    }
    return true;
  }

  // The player's current best rating (= score on Point boards) for a board, if known.
  bool PersonalBest(uint32_t view_id, int64_t& out) {
    std::lock_guard<std::mutex> lock(bests_mutex_);
    auto it = bests_.find(view_id);
    if (it == bests_.end()) return false;
    out = it->second;
    return true;
  }

 private:
  // Fetches the player's bests for every board (worker thread).
  void RefreshBests() {
    std::string xuid = REXCVAR_GET(user_xuid);
    if (xuid.empty()) return;
    for (auto& c : xuid) c = char(std::tolower(static_cast<unsigned char>(c)));
    auto r = Call("get_player_bests", json{{"p_xuid", xuid}}.dump());
    if (r.status != 200) {
      REXLOG_WARN("e4 leaderboard: couldn't fetch your bests: {}",
                  r.status ? std::to_string(r.status) : r.error);
      return;
    }
    json rows = json::parse(r.body, nullptr, false);
    if (!rows.is_array()) return;
    std::unordered_map<uint32_t, int64_t> bests;
    for (const auto& j : rows) {
      if (j.is_object()) bests[j.value("board", 0u)] = j.value("rating", int64_t(0));
    }
    REXLOG_INFO("e4 leaderboard: your bests loaded for {} board(s)", bests.size());
    std::lock_guard<std::mutex> lock(bests_mutex_);
    bests_ = std::move(bests);
  }

  e4::http::Response Call(const std::string& function, const std::string& body) {
    return e4::http::Post(url_ + "/rest/v1/rpc/" + function,
                          {{"apikey", key_},
                           {"Authorization", "Bearer " + key_},
                           {"Content-Type", "application/json"}},
                          body, kTimeoutMs);
  }

  void Run() {
    RefreshBests();
    std::unique_lock<std::mutex> lock(mutex_);
    bool sent_since_refresh = false;
    for (;;) {
      if (queue_.empty()) {
        if (sent_since_refresh) {
          // New scores are in: update the bests the HUD shows.
          sent_since_refresh = false;
          lock.unlock();
          RefreshBests();
          lock.lock();
          continue;
        }
        cv_.wait(lock);
        continue;
      }
      std::string body = queue_.front();
      sending_ = true;
      lock.unlock();
      auto r = Call("submit_score", body);
      lock.lock();
      sending_ = false;
      queue_.pop_front();
      if (r.status >= 200 && r.status < 300) {
        REXLOG_INFO("e4 leaderboard: score sent");
        sent_since_refresh = true;
      } else if (r.status == 0 || r.status >= 500 || r.status == 429) {
        // Offline / server trouble: keep it for the next start.
        REXLOG_WARN("e4 leaderboard: couldn't send a score ({}), saved for later",
                    r.status ? std::to_string(r.status) : r.error);
        std::ofstream(pending_path_, std::ios::app) << body << "\n";
      } else {
        REXLOG_WARN("e4 leaderboard: server refused a score: {} {}", r.status,
                    r.body.substr(0, 200));
      }
      cv_.notify_all();
    }
  }

  void LoadPending() {
    std::ifstream in(pending_path_);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty()) queue_.push_back(line);
    }
    in.close();
    if (!queue_.empty()) {
      REXLOG_INFO("e4 leaderboard: retrying {} saved score(s)", queue_.size());
      std::error_code ec;
      std::filesystem::remove(pending_path_, ec);
    }
  }

  std::string url_, key_, token_;
  std::filesystem::path pending_path_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  bool sending_ = false;
  std::thread worker_;
  std::mutex bests_mutex_;
  std::unordered_map<uint32_t, int64_t> bests_;
};

std::atomic<SupabaseStats*> g_provider{nullptr};

// Game state (guest addresses).
constexpr uint32_t kHudHiScore = 0x82851E98;     // u64 HI-SCORE shown top right
constexpr uint32_t kRunningScore = 0x883F7B88;   // u64 score shown while playing
constexpr uint32_t kBoardKey = 0x88349860 + 8860;  // u32 mode/stage key -> stats view

}  // namespace

void Install() {
  if (!REXCVAR_GET(e4_leaderboards)) {
    REXLOG_INFO("e4 leaderboard: online leaderboards off");
    return;
  }
  static SupabaseStats* provider = new SupabaseStats();  // lives until exit
  g_provider = provider;
  xam::SetStatsProvider(provider);
  REXLOG_INFO("e4 leaderboard: online leaderboards on ({})",
              std::string(REXCVAR_GET(e4_leaderboard_url)));
}

}  // namespace e4::leaderboard

// Per-frame score update while playing. When a play starts the game puts the local
// top-10's #1 into the HI-SCORE; while the score is still 0, replace it with the
// player's online best for this board. The game then raises it as usual once beaten.
// Music sequencer (beat meter state at 0x8284BB58; 960 ticks per beat).
//
// The sequencer runs on wall-clock music time, while the music itself reaches the
// speakers ~210 ms after its beat (measured with sync_record: the heard kick and the
// player's taps both land a fixed ~210-250 ms after the sequencer's beat at every
// tempo). e4_beat_offset_ms runs the sequencer that much behind, so the bar, beat lamps
// and beat timing follow the music as heard. Only the sequencer's own reads of the
// music clock are shifted; everything else (e.g. scheduling the music) is unchanged.
//
// Two updaters, chosen by the caller (0x822582F0): sub_8228E8D0 (normal play) reads
// the music stopwatch at 0x88402FB0 through sub_822500A0; sub_8228EC08 (other modes)
// uses an accumulator of frame times at 0x828B99C0.
namespace {
constexpr uint32_t kMusicStopwatch = 0x88402FB0;
thread_local bool g_in_sequencer = false;

// Beat length of the current tempo, seconds (sequencer +40).
double BeatLengthSeconds(uint8_t* base) {
  const double len = std::bit_cast<double>(REX_LOAD_U64(0x8284BB58 + 40));
  return (len > 0.1 && len < 2.0) ? len : 0.0;
}
double BeatOffsetSeconds(uint8_t* base) {
  return REXCVAR_GET(e4_beat_offset_beats) * BeatLengthSeconds(base) +
         REXCVAR_GET(e4_beat_offset_ms) / 1000.0;
}

void RecordBeat(uint8_t* base) {
  if (!rex::sync_record::Enabled()) return;
  // Beat recording: the sequencer state after each update, on the sync clock.
  static std::FILE* beat_file = [] {
    std::FILE* f = std::fopen("sync_beat.csv", "w");
    if (f) {
      std::fprintf(f, "game_ms");
      for (int i = 0; i < 32; ++i) std::fprintf(f, ",w%d", i * 4);
      std::fprintf(f, "\n");
    }
    return f;
  }();
  if (!beat_file) return;
  std::fprintf(beat_file, "%.3f", double(rex::sync_record::NowNs()) / 1e6);
  for (uint32_t i = 0; i < 32; ++i) {
    std::fprintf(beat_file, ",%08X", REX_LOAD_U32(0x8284BB58 + i * 4));
  }
  std::fprintf(beat_file, "\n");
  static int lines = 0;
  if (++lines % 60 == 0) std::fflush(beat_file);
}
}  // namespace

// Timing diagnostics (sync_record): the music stopwatch's reset / restart / pause / resume
// and music pattern starts, with the caller, to sync_events.csv.
namespace {
void ClockEvent(const char* what, uint32_t a, double b) {
  if (!rex::sync_record::Enabled()) return;
  rex::sync_record::Write(rex::sync_record::Stream::kEvents,
                          fmt::format("{:.3f},clock,{},{:08X},{:.4f}",
                                      rex::sync_record::NowNs() / 1e6, what, a, b));
}
double RawMusicClock(PPCContext& ctx, uint8_t* base) {
  PPCContext tmp = ctx;
  tmp.r3.u64 = kMusicStopwatch;
  __imp__sub_822500A0(tmp, base);
  return tmp.f1.f64;
}
}  // namespace

// Stopwatch read (r3 = stopwatch) -> f1 seconds.
//
// The music stopwatch restarts at every loop of the music (4 bars, ~7 s). Running the
// sequencer `offset` behind means that for the first `offset` seconds after a restart
// it must still be finishing the previous loop - so those reads map to the end of the
// previous loop (its length measured at the restart) instead of going negative, which
// froze the beat at the start of each loop and skipped the end of the last one.
extern "C" REX_FUNC(sub_822500A0) {
  const uint32_t stopwatch = ctx.r3.u32;
  __imp__sub_822500A0(ctx, base);
  if (!g_in_sequencer || stopwatch != kMusicStopwatch) return;
  static double last_raw = -1.0;      // previous unshifted reading
  static int64_t last_host_ns = 0;    // when it was taken
  static double previous_loop = 0.0;  // length of the loop before the latest restart
  const double raw = ctx.f1.f64;
  const int64_t now_ns = rex::sync_record::NowNs();
  if (last_raw >= 0.0 && raw + 0.5 < last_raw) {
    // Restarted: the loop ran until (last reading + time since it) - (time into the new one).
    previous_loop = last_raw + double(now_ns - last_host_ns) / 1e9 - raw;
    ClockEvent("wrap", uint32_t(previous_loop * 1e6), raw);
  }
  last_raw = raw;
  last_host_ns = now_ns;
  double shifted = raw - BeatOffsetSeconds(base);
  if (shifted < 0.0 && previous_loop > 0.0) {
    shifted += previous_loop;  // still in the previous loop
  }
  ctx.f1.f64 = shifted;
}


#define E4_CLOCK_HOOK(fn, name)                                        \
  extern "C" REX_FUNC(fn) {                                            \
    if (ctx.r3.u32 == kMusicStopwatch) {                               \
      ClockEvent(name, uint32_t(ctx.lr), RawMusicClock(ctx, base));    \
    }                                                                  \
    __imp__##fn(ctx, base);                                            \
  }
E4_CLOCK_HOOK(sub_82250000, "reset")
E4_CLOCK_HOOK(sub_82250110, "restart")
E4_CLOCK_HOOK(sub_82250198, "resume")
E4_CLOCK_HOOK(sub_82250228, "pause")
#undef E4_CLOCK_HOOK

extern "C" REX_FUNC(sub_8228E688) {
  ClockEvent("pattern", ctx.r3.u32, RawMusicClock(ctx, base));
  __imp__sub_8228E688(ctx, base);
}

extern "C" REX_FUNC(sub_8228E8D0) {
  g_in_sequencer = true;
  __imp__sub_8228E8D0(ctx, base);
  g_in_sequencer = false;
  RecordBeat(base);
}

extern "C" REX_FUNC(sub_8228EC08) {
  constexpr uint32_t kMusicTime = 0x828B99C0;  // double, seconds
  constexpr uint32_t kSeqState = 0x828B980C;   // 4 = music time from the accumulator
  const bool shift = BeatOffsetSeconds(base) != 0.0 && REX_LOAD_U32(kSeqState) == 4;
  double applied = 0.0;
  if (shift) {
    // Same loop handling as the stopwatch path: early in a new loop, keep finishing the
    // previous one rather than going negative.
    static double last_raw = -1.0, previous_loop = 0.0;
    const double raw = std::bit_cast<double>(REX_LOAD_U64(kMusicTime));
    if (last_raw >= 0.0 && raw + 0.5 < last_raw) previous_loop = last_raw - raw;
    last_raw = raw;
    applied = BeatOffsetSeconds(base);
    if (raw - applied < 0.0 && previous_loop > 0.0) applied -= previous_loop;
    REX_STORE_U64(kMusicTime, std::bit_cast<uint64_t>(raw - applied));
  }
  __imp__sub_8228EC08(ctx, base);
  if (shift) {
    REX_STORE_U64(kMusicTime, std::bit_cast<uint64_t>(
                                  std::bit_cast<double>(REX_LOAD_U64(kMusicTime)) + applied));
  }
  RecordBeat(base);
}

extern "C" REX_FUNC(sub_8227C030) {
  using namespace e4::leaderboard;
  auto* provider = g_provider.load(std::memory_order_relaxed);
  if (provider && REXCVAR_GET(e4_hud_online_best) && REX_LOAD_U64(kRunningScore) == 0) {
    // Board key -> stats view, via the game's own mapping (cached per key).
    static std::array<uint32_t, 64> view_of_key = {};
    const uint32_t key = REX_LOAD_U32(kBoardKey);
    uint32_t view = 0;
    if (key < view_of_key.size()) {
      if (!view_of_key[key]) {
        PPCContext tmp = ctx;
        tmp.r3.u64 = key;
        sub_82346B58(tmp, base);
        view_of_key[key] = tmp.r3.u32 ? tmp.r3.u32 : 0xFFFFFFFF;
      }
      view = view_of_key[key] == 0xFFFFFFFF ? 0 : view_of_key[key];
    }
    int64_t best = 0;
    if (view && provider->PersonalBest(view, best) && best > 0 &&
        REX_LOAD_U64(kHudHiScore) != uint64_t(best)) {
      REX_STORE_U64(kHudHiScore, uint64_t(best));
      static uint64_t logged = 0;
      if (logged != (uint64_t(view) << 56 ^ uint64_t(best))) {
        logged = uint64_t(view) << 56 ^ uint64_t(best);
        REXLOG_INFO("e4 leaderboard: HI-SCORE set to your online best {} (board {})", best,
                    view);
      }
    }
  }
  __imp__sub_8227C030(ctx, base);
}
