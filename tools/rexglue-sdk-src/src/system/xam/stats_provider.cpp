/**
 * @file        system/xam/stats_provider.cpp
 * @brief       Registration point for the Xbox LIVE stats backend, plus the guest
 *              XUSER_STATS_READ_RESULTS writer shared by the stats enumerator and
 *              XUserReadStats.
 */

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

#include <rex/memory/utils.h>
#include <rex/system/xam/stats_provider.h>

namespace rex::system::xam {

namespace {
std::atomic<StatsProvider*> g_stats_provider{nullptr};

constexpr uint32_t kResultsSize = 8;
constexpr uint32_t kViewSize = 16;
constexpr uint32_t kRowSize = 48;
constexpr uint32_t kColumnSize = 24;
}  // namespace

void SetStatsProvider(StatsProvider* provider) {
  g_stats_provider.store(provider, std::memory_order_release);
}

StatsProvider* GetStatsProvider() {
  return g_stats_provider.load(std::memory_order_acquire);
}

StatsSpec ReadGuestStatsSpec(const uint8_t* spec) {
  using rex::memory::load_and_swap;
  StatsSpec out;
  out.view_id = load_and_swap<uint32_t>(spec + 0);
  uint32_t cols = std::min<uint32_t>(load_and_swap<uint32_t>(spec + 4), 64);
  for (uint32_t c = 0; c < cols; ++c) {
    out.column_ids.push_back(load_and_swap<uint16_t>(spec + 8 + c * 2));
  }
  return out;
}

uint32_t StatsResultsSize(const std::vector<StatsSpec>& specs, uint32_t rows_per_view) {
  uint32_t size = kResultsSize;
  for (const auto& spec : specs) {
    size += kViewSize +
            rows_per_view * (kRowSize + uint32_t(spec.column_ids.size()) * kColumnSize);
  }
  return size;
}

void WriteGuestStatsResults(uint8_t* data, uint32_t guest_ptr,
                            const std::vector<StatsSpec>& specs,
                            std::vector<StatsViewResult>& views, uint32_t max_rows) {
  using rex::memory::store_and_swap;
  std::memset(data, 0, StatsResultsSize(specs, max_rows));
  const uint32_t views_offset = kResultsSize;
  uint32_t next = views_offset + uint32_t(specs.size()) * kViewSize;
  store_and_swap<uint32_t>(data + 0, uint32_t(specs.size()));
  store_and_swap<uint32_t>(data + 4, specs.empty() ? 0 : guest_ptr + views_offset);
  for (size_t v = 0; v < specs.size(); ++v) {
    const auto& spec = specs[v];
    auto& result = views[v];
    if (result.rows.size() > max_rows) {
      result.rows.resize(max_rows);
    }
    uint8_t* view = data + views_offset + v * kViewSize;
    uint32_t rows_offset = next;
    next += uint32_t(result.rows.size()) * kRowSize;
    store_and_swap<uint32_t>(view + 0, spec.view_id);
    store_and_swap<uint32_t>(view + 4, std::max(result.total_rows, uint32_t(result.rows.size())));
    store_and_swap<uint32_t>(view + 8, uint32_t(result.rows.size()));
    store_and_swap<uint32_t>(view + 12, result.rows.empty() ? 0 : guest_ptr + rows_offset);
    for (size_t r = 0; r < result.rows.size(); ++r) {
      const auto& row = result.rows[r];
      uint8_t* out = data + rows_offset + r * kRowSize;
      store_and_swap<uint64_t>(out + 0, row.xuid);
      store_and_swap<uint32_t>(out + 8, row.rank);
      store_and_swap<uint64_t>(out + 16, uint64_t(row.rating));
      std::memcpy(out + 24, row.gamertag.data(), std::min<size_t>(row.gamertag.size(), 15));
      // Columns in the order the spec asked for them.
      uint32_t cols_offset = next;
      next += uint32_t(spec.column_ids.size()) * kColumnSize;
      store_and_swap<uint32_t>(out + 40, uint32_t(spec.column_ids.size()));
      store_and_swap<uint32_t>(out + 44, spec.column_ids.empty() ? 0 : guest_ptr + cols_offset);
      for (size_t c = 0; c < spec.column_ids.size(); ++c) {
        uint8_t* col = data + cols_offset + c * kColumnSize;
        store_and_swap<uint16_t>(col + 0, spec.column_ids[c]);
        for (const auto& have : row.columns) {
          if (have.column_id != spec.column_ids[c]) continue;
          col[8] = uint8_t(have.value.type);
          switch (have.value.type) {
            case StatsDataType::kInt32:
              store_and_swap<uint32_t>(col + 16, uint32_t(int32_t(have.value.i64)));
              break;
            case StatsDataType::kInt64:
              store_and_swap<uint64_t>(col + 16, uint64_t(have.value.i64));
              break;
            case StatsDataType::kDouble:
              store_and_swap<uint64_t>(col + 16, std::bit_cast<uint64_t>(have.value.dbl));
              break;
            default:
              break;
          }
          break;
        }
      }
    }
  }
}

}  // namespace rex::system::xam
