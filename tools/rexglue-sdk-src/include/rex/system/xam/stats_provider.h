#pragma once
/**
 * @file        system/xam/stats_provider.h
 * @brief       Pluggable backend for Xbox LIVE stats (leaderboards).
 *
 * The XAM layer turns the title's XSessionWriteStats calls into WriteStats(), and
 * XUserCreateStatsEnumeratorBy* + XEnumerate into ReadStats(). A project registers
 * its own provider (e.g. an online leaderboard service) with SetStatsProvider().
 * Without a provider, writes are dropped and reads return empty boards, so the
 * title's leaderboard screens still finish loading.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace rex::system::xam {

// XUSER_DATA types used by stats.
enum class StatsDataType : uint8_t {
  kNone = 0,
  kInt32 = 1,
  kInt64 = 2,
  kDouble = 3,
};

struct StatsValue {
  StatsDataType type = StatsDataType::kNone;
  int64_t i64 = 0;   // kInt32 / kInt64
  double dbl = 0.0;  // kDouble
};

struct StatsProperty {
  uint32_t property_id;
  StatsValue value;
};

struct StatsColumn {
  uint16_t column_id;  // the view's attribute id
  StatsValue value;
};

struct StatsRow {
  uint64_t xuid = 0;
  uint32_t rank = 0;  // 1-based
  int64_t rating = 0;
  std::string gamertag;
  std::vector<StatsColumn> columns;
};

struct StatsSpec {
  uint32_t view_id = 0;
  std::vector<uint16_t> column_ids;
};

struct StatsViewResult {
  uint32_t view_id = 0;
  uint32_t total_rows = 0;
  std::vector<StatsRow> rows;
};

enum class StatsReadMode : uint32_t {
  kByXuid = 0,    // rows around the player `pivot`
  kByRank = 1,    // rows starting at rank `pivot` (1-based)
  kByRating = 2,  // rows around rating `pivot`
};

class StatsProvider {
 public:
  virtual ~StatsProvider() = default;
  // Called from the title's thread; must not block for long (queue network work).
  virtual void WriteStats(uint64_t xuid, const std::string& gamertag, uint32_t view_id,
                          const std::vector<StatsProperty>& properties) = 0;
  // Called from the deferred-completion worker (XEnumerate); may block briefly.
  // Returns false on failure (the board is then shown empty).
  virtual bool ReadStats(StatsReadMode mode, uint64_t pivot, uint32_t num_rows,
                         const StatsSpec& spec, StatsViewResult& out) = 0;
};

// The provider is not owned; it must outlive the title.
void SetStatsProvider(StatsProvider* provider);
StatsProvider* GetStatsProvider();

// Guest XUSER_STATS_READ_RESULTS helpers (big-endian, pointers into one buffer):
//   RESULTS  8: num_views, views*
//   VIEW    16: view_id, total_rows, num_rows, rows*
//   ROW     48: xuid (8), rank, pad, rating (8), gamertag[16], num_cols, cols*
//   COLUMN  24: column_id (2), pad (6), XUSER_DATA { type, pad[7], value (8) }
//   SPEC   136: view_id, num_columns, column_ids[64] (u16)  (input)
inline constexpr uint32_t kStatsSpecSize = 136;
StatsSpec ReadGuestStatsSpec(const uint8_t* spec);
uint32_t StatsResultsSize(const std::vector<StatsSpec>& specs, uint32_t rows_per_view);
// Writes the results for specs[i] / views[i] (views[i].rows truncated to max_rows) into
// `data`, which the guest sees at `guest_ptr`. The buffer must hold StatsResultsSize().
void WriteGuestStatsResults(uint8_t* data, uint32_t guest_ptr,
                            const std::vector<StatsSpec>& specs,
                            std::vector<StatsViewResult>& views, uint32_t max_rows);

}  // namespace rex::system::xam
