// e4 - extra explosion particles.
//
// The game draws explosions through two recompiled functions:
//   sub_82338770  burst/ring sprite: r3 draw ctx, f1/f2 position, f3/f4 size,
//                 f5 rotation, r8 colour (0xFFRRGGBB, additive), r10 0=burst 1=ring,
//                 caller stack +92 = explosion texture variant.
//   sub_82339460  60-frame animated explosion: r4 frame (= age), f1-f5 as above,
//                 r10 colour.
// We override both (the generated versions are weak; __imp__ is the original), call
// the original, then draw extra spark sprites with the game's own burst sprite:
// sparks fly outward, shrink and fade; rings get a faint expanding shockwave echo.

#include "generated/default/e4_init.h"
#include "e4_glow.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <atomic>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <xxhash.h>

namespace e4 {
rex::memory::Memory* g_memory = nullptr;  // set by E4App once the runtime exists
}

REXCVAR_DEFINE_BOOL(e4_extra_particles, true, "E4", "Extra explosion particles");
REXCVAR_DEFINE_INT32(e4_particle_count, 12, "E4", "Sparks per explosion (0-32)");
REXCVAR_DEFINE_BOOL(e4_sparks_on_large_explosions, false, "E4",
                    "Also add sparks to the larger (stage / animated) explosions");
REXCVAR_DEFINE_INT32(e4_spark_budget, 600, "E4",
                     "Max spark sprites per frame; in big chains each explosion gets fewer");
REXCVAR_DEFINE_BOOL(e4_sprite_stats, false, "E4", "Log which code draws sprites (diagnostic)");
REXCVAR_DEFINE_BOOL(e4_bloom_gameplay_only, true, "E4", "Only apply the glow during gameplay");

// Diagnostic: histogram of sprite draws (sub_82343E38) by caller and texture.
namespace {
std::mutex g_stats_mutex;
std::unordered_map<uint64_t, uint32_t> g_stats;
std::chrono::steady_clock::time_point g_stats_time = std::chrono::steady_clock::now();
uint32_t g_hook_burst = 0, g_hook_anim = 0, g_sparks_drawn = 0;
std::unordered_map<uint32_t, std::string> g_texture_names;  // handle -> file name
std::unordered_map<uint32_t, bool> g_texture_hashed_on_draw;
}  // namespace
uint64_t TextureHash(uint8_t* base, uint32_t tex, uint32_t& w, uint32_t& h);
namespace {
}  // namespace

// Texture loader (r3 resource, r4 name) -> r3 texture handle. Records names and
// replacement hashes (texture_names.csv, sprite glow lookup, sprite stats).
extern "C" REX_FUNC(sub_823B6488) {
  std::string name;
  if (ctx.r4.u32) {
    for (uint32_t p = ctx.r4.u32; name.size() < 96; ++p) {
      char c = char(REX_LOAD_U8(p));
      if (!c) break;
      name.push_back(c);
    }
  }
  __imp__sub_823B6488(ctx, base);
  if (name.empty()) return;
  uint32_t w = 0, h = 0;
  uint64_t hash = TextureHash(base, ctx.r3.u32, w, h);
  {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    g_texture_names[ctx.r3.u32] = name;
  }
  e4::glow::OnTextureLoaded(ctx.r3.u32, name, w, h, hash);
  if (REXCVAR_GET(e4_sprite_stats)) {
    REXLOG_INFO("texture {:08X} = {} {}x{} hash {:016x}", ctx.r3.u32, name, w, h, hash);
  }
}

// Same hash the texture replacement uses (see texture_cache.cpp), so dumped
// PNGs (<w>x<h>_<fmt>_<hash>.png) can be matched to texture names.
uint64_t TextureHash(uint8_t* base, uint32_t tex, uint32_t& w, uint32_t& h) {
  uint64_t hash = 0;
  if (tex && e4::g_memory) {
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
    uint32_t* dw = &fetch.dword_0;
    for (int i = 0; i < 6; ++i) dw[i] = REX_LOAD_U32(tex + 0x1C + i * 4);
    if (fetch.type == rex::graphics::xenos::FetchConstantType::kTexture) {
      uint32_t wm1, hm1, dm1, base_page, mip_page, mip_max;
      rex::graphics::texture_util::GetSubresourcesFromFetchConstant(
          fetch, &wm1, &hm1, &dm1, &base_page, &mip_page, nullptr, &mip_max);
      auto format = rex::graphics::GetBaseFormat(fetch.format);
      w = wm1 + 1;
      h = hm1 + 1;
      if (base_page) {
        auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
            fetch.dimension, fetch.pitch, w, h, dm1 + 1, fetch.tiled, format, fetch.packed_mips,
            true, mip_max);
        // The fetch constant the GPU ends up using points one 4 KB page past the
        // header copy (measured against the texture cache).
        const uint8_t* data = e4::g_memory->TranslatePhysical<const uint8_t*>((base_page + 1) << 12);
        uint64_t seed = (uint64_t(uint32_t(format)) << 48) ^ (uint64_t(w) << 24) ^ uint64_t(h);
        hash = XXH3_64bits_withSeed(data, layout.base.level_data_extent_bytes, seed);
        REXLOG_DEBUG("tex {:08X} base {:08X} size {} fmt {} pitch {} tiled {} packed {} mips {}", tex, base_page << 12,
                     layout.base.level_data_extent_bytes, uint32_t(format), uint32_t(fetch.pitch), uint32_t(fetch.tiled), uint32_t(fetch.packed_mips), mip_max);
      }
    }
  }
  return hash;
}

// --- Glow only during gameplay ------------------------------------------------
// The GPU bloom cvar lives in the GPU plugin; we flip it by name. sub_822D93F0
// (the stage's explosion renderer) runs every gameplay frame and never in the
// menus, so "it ran in the last 300 ms" means we're in a stage.
namespace {
std::atomic<int64_t> g_last_gameplay_ms{-1000000};
int g_bloom_wanted = -1;  // -1 = not read yet; else whether the user enabled bloom
bool g_bloom_on = true;
uint32_t g_bloom_check_counter = 0;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void UpdateBloom(bool in_gameplay) {
  if (g_bloom_wanted < 0) {
    g_bloom_wanted = rex::cvar::GetFlagByName("bloom") == "true" ? 1 : 0;
    g_bloom_on = g_bloom_wanted == 1;
  }
  bool want = g_bloom_wanted == 1 && (in_gameplay || !REXCVAR_GET(e4_bloom_gameplay_only));
  if (want != g_bloom_on) {
    g_bloom_on = want;
    rex::cvar::SetFlagByName("bloom", want ? "true" : "false");
    REXLOG_INFO("e4: glow {}", want ? "on (gameplay)" : "off (menus)");
  }
}
}  // namespace

extern "C" REX_FUNC(sub_822D93F0) {
  g_last_gameplay_ms.store(NowMs(), std::memory_order_relaxed);
  UpdateBloom(true);
  __imp__sub_822D93F0(ctx, base);
}

extern "C" REX_FUNC(sub_82343E38) {
  // Sprites are drawn every frame in menus too; check now and then whether
  // gameplay has stopped.
  if ((++g_bloom_check_counter & 63) == 0) {
    UpdateBloom(NowMs() - g_last_gameplay_ms.load(std::memory_order_relaxed) < 300);
  }
  if (REXCVAR_GET(e4_sprite_stats)) {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    g_stats[(uint64_t(ctx.lr & 0xFFFFFFFF) << 32) | ctx.r5.u32]++;
    if (g_texture_hashed_on_draw.try_emplace(ctx.r5.u32, true).second) {
      uint32_t w = 0, h = 0;
      uint64_t hash = TextureHash(base, ctx.r5.u32, w, h);
      auto it = g_texture_names.find(ctx.r5.u32);
      REXLOG_INFO("drawn texture {:08X} = {} {}x{} hash {:016x}", ctx.r5.u32,
                  it != g_texture_names.end() ? it->second : std::string("?"), w, h, hash);
    }
    auto now = std::chrono::steady_clock::now();
    if (now - g_stats_time > std::chrono::seconds(5)) {
      g_stats_time = now;
      std::vector<std::pair<uint64_t, uint32_t>> v(g_stats.begin(), g_stats.end());
      std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
      REXLOG_INFO("sprite stats: burst hook {} anim hook {} sparks {} ({} caller/texture pairs)",
                  g_hook_burst, g_hook_anim, g_sparks_drawn, v.size());
      for (size_t i = 0; i < v.size() && i < 40; ++i)
        REXLOG_INFO("  caller {:08X} tex {:08X}: {}", uint32_t(v[i].first >> 32), uint32_t(v[i].first),
                    v[i].second);
      g_stats.clear();
      g_hook_burst = g_hook_anim = g_sparks_drawn = 0;
    }
  }
  e4::glow::SpriteScaleUndo undo;
  if (e4::glow::BeginSprite(ctx, base, undo)) {
    __imp__sub_82343E38(ctx, base);
    e4::glow::EndSprite(base, undo);
  } else {
    __imp__sub_82343E38(ctx, base);
  }
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr float kTwoPi = 6.28318530718f;

struct Tracked {
  Clock::time_point first_seen;
  Clock::time_point last_seen;
  float max_size = 0.0f;
  uint32_t peak_colour = 0;  // per-channel max seen
};

uint32_t MaxColour(uint32_t a, uint32_t b) {
  uint32_t out = 0xFF000000u;
  for (int sh = 0; sh < 24; sh += 8) out |= std::max((a >> sh) & 0xFF, (b >> sh) & 0xFF) << sh;
  return out;
}

std::mutex g_mutex;
std::unordered_map<uint64_t, Tracked> g_tracked;
Clock::time_point g_last_sweep;

// Per-frame spark budget so huge chains can't flood the GPU with draws (every
// spark is its own draw). Shared fairly: the sparks per explosion are scaled
// down from how many explosions wanted sparks in the previous frame.
Clock::time_point g_budget_window;
int g_budget_used = 0;
int g_spark_requests = 0;       // explosions asking for sparks this frame
int g_spark_requests_last = 0;  // ... and in the previous frame

bool g_in_hook = false;  // our own extra draws must not spawn more sparks

uint32_t Hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352d;
  x ^= x >> 15; x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}
float Rand01(uint32_t& s) {
  s = Hash32(s + 0x9e3779b9);
  return float(s & 0xFFFFFF) / float(0x1000000);
}

uint32_t FloatBits(double v) {
  float f = float(v);
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

uint32_t ScaleColour(uint32_t argb, float k) {
  if (k <= 0.0f) return argb & 0xFF000000u;
  if (k > 1.0f) k = 1.0f;
  uint32_t r = uint32_t(((argb >> 16) & 0xFF) * k);
  uint32_t g = uint32_t(((argb >> 8) & 0xFF) * k);
  uint32_t b = uint32_t((argb & 0xFF) * k);
  return (argb & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

bool TakeBudget(int n) {
  auto now = Clock::now();
  if (now - g_budget_window > std::chrono::milliseconds(12)) {
    g_budget_window = now;
    g_budget_used = 0;
    g_spark_requests_last = g_spark_requests;
    g_spark_requests = 0;
  }
  if (g_budget_used + n > std::max(0, int(REXCVAR_GET(e4_spark_budget)))) return false;
  g_budget_used += n;
  return true;
}

struct SavedArgs {
  uint64_t r1, r3, r8, r10;
  double f1, f2, f3, f4, f5;
};
SavedArgs Save(PPCContext& c) {
  return {c.r1.u64, c.r3.u64, c.r8.u64, c.r10.u64, c.f1.f64, c.f2.f64, c.f3.f64, c.f4.f64, c.f5.f64};
}

// One extra burst sprite through the game's original routine.
// rot is in degrees (the game wraps angles with fmod 360).
void DrawSprite(PPCContext& ctx, uint8_t* base, const SavedArgs& a, double x, double y,
                double size_x, double size_y, double rot, uint32_t colour, bool ring) {
  ctx.r1.u64 = a.r1;
  ctx.r3.u64 = a.r3;
  ctx.r8.u64 = colour;
  ctx.r10.u64 = ring ? 1 : 0;
  ctx.f1.f64 = x;
  ctx.f2.f64 = y;
  ctx.f3.f64 = size_x;
  ctx.f4.f64 = size_y;
  ctx.f5.f64 = rot;
  __imp__sub_82338770(ctx, base);
}

// Sparks for one explosion of the given age (in 60 Hz frames).
void DrawSparks(PPCContext& ctx, uint8_t* base, const SavedArgs& a, uint32_t seed,
                float age, float size, uint32_t colour) {
  int count = REXCVAR_GET(e4_particle_count);
  if (count <= 0 || size <= 0.0f) return;
  if (count > 32) count = 32;
  ++g_spark_requests;
  if (g_spark_requests_last > 0) {
    int fair = REXCVAR_GET(e4_spark_budget) / g_spark_requests_last;
    count = std::min(count, std::max(fair, 3));
  }
  if (!TakeBudget(count)) return;
  // Sparks use the game's soft round glow sprite (texture variant 0,
  // GenericExplosion), which the burst routine reads from the caller's outgoing
  // argument area (+92). Swap it in for our draws and restore it afterwards.
  const uint32_t r1 = uint32_t(a.r1);
  const uint32_t saved_variant = REX_LOAD_U32(r1 + 92);
  REX_STORE_U32(r1 + 92, 0);
  for (int i = 0; i < count; ++i) {
    uint32_t s = seed * 31u + uint32_t(i) * 7919u;
    float angle = (float(i) + Rand01(s) * 0.8f) * kTwoPi / float(count);
    float speed = 0.8f + Rand01(s) * 1.2f;        // how far it flies, in explosion sizes
    float life = 30.0f + Rand01(s) * 30.0f;       // frames
    float length = 0.16f + Rand01(s) * 0.14f;     // streak length vs explosion
    float width = 0.025f + Rand01(s) * 0.02f;     // streak thickness vs explosion
    float t = age / life;
    if (t >= 1.0f) continue;
    float ease = 1.0f - (1.0f - t) * (1.0f - t);
    float radius = size * (0.15f + speed * ease);
    double x = a.f1 + std::cos(angle) * radius;
    double y = a.f2 + std::sin(angle) * radius;
    // Thin streak pointing along its flight; it shortens as it slows down.
    float velocity = 2.0f * (1.0f - t);  // d(ease)/dt
    float len = size * length * (0.35f + 0.65f * velocity * 0.5f);
    float wid = size * width * (1.0f - 0.4f * t);
    float bright = std::pow(1.0f - t, 1.2f);
    DrawSprite(ctx, base, a, x, y, len, wid, angle * (360.0f / kTwoPi),
               ScaleColour(colour, bright), false);
    ++g_sparks_drawn;
  }
  REX_STORE_U32(r1 + 92, saved_variant);
}

void Sweep(Clock::time_point now) {
  if (now - g_last_sweep < std::chrono::milliseconds(250)) return;
  g_last_sweep = now;
  for (auto it = g_tracked.begin(); it != g_tracked.end();) {
    if (now - it->second.last_seen > std::chrono::milliseconds(300)) it = g_tracked.erase(it);
    else ++it;
  }
}

}  // namespace

// Burst / ring explosion sprite.
extern "C" REX_FUNC(sub_82338770) {
  if (!g_in_hook) ++g_hook_burst;
  if (g_in_hook || !REXCVAR_GET(e4_extra_particles)) {
    __imp__sub_82338770(ctx, base);
    return;
  }
  const SavedArgs a = Save(ctx);
  // Texture variant (caller's outgoing argument area +92): 0 = GenericExplosion,
  // the round burst. Other variants are the larger stage explosions.
  const uint32_t variant = REX_LOAD_U32(uint32_t(a.r1) + 92);
  // The texture-variant argument lives in the caller's frame; keep it intact.
  __imp__sub_82338770(ctx, base);

  const bool ring = (a.r10 & 0xFFFFFFFF) != 0;
  const uint32_t colour = uint32_t(a.r8);
  const uint64_t key = (uint64_t(FloatBits(a.f1)) << 32 | FloatBits(a.f2)) ^ (ring ? 0x5bd1e995ull : 0);
  float age, max_size;
  uint32_t peak_colour;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto now = Clock::now();
    Sweep(now);
    auto [it, inserted] = g_tracked.try_emplace(key);
    if (inserted) it->second.first_seen = now;
    it->second.last_seen = now;
    it->second.max_size = std::max(it->second.max_size, float(a.f3));
    age = std::chrono::duration<float>(now - it->second.first_seen).count() * 60.0f;
    max_size = it->second.max_size;
    it->second.peak_colour = MaxColour(it->second.peak_colour, colour);
    peak_colour = it->second.peak_colour;
  }

  g_in_hook = true;
  if (ring) {
    // Shockwave echo: a second ring that keeps expanding past the first and fades.
    constexpr float kEchoLife = 34.0f;
    float t = age / kEchoLife;
    if (t < 1.0f && TakeBudget(1)) {
      float ease = 1.0f - (1.0f - t) * (1.0f - t);
      double echo = max_size * (1.0f + 0.7f * ease);
      DrawSprite(ctx, base, a, a.f1, a.f2, echo, echo, a.f5,
                 ScaleColour(colour, 0.45f * (1.0f - t)), true);
    }
  } else if (variant == 0 || REXCVAR_GET(e4_sparks_on_large_explosions)) {
    // Sparks only on the round burst; the larger explosions don't need them.
    DrawSparks(ctx, base, a, Hash32(uint32_t(key) ^ uint32_t(key >> 32)), age, max_size,
               peak_colour);
  }
  g_in_hook = false;
  // Leave the argument registers as the original call would have.
  ctx.r1.u64 = a.r1;
}

// 60-frame animated explosion: r4 is the frame, i.e. the explosion's age.
extern "C" REX_FUNC(sub_82339460) {
  if (!g_in_hook) ++g_hook_anim;
  if (g_in_hook || !REXCVAR_GET(e4_extra_particles)) {
    __imp__sub_82339460(ctx, base);
    return;
  }
  const SavedArgs a0 = Save(ctx);
  const uint32_t frame = ctx.r4.u32;
  const uint32_t colour = ctx.r10.u32;
  __imp__sub_82339460(ctx, base);
  if (frame > 59 || !REXCVAR_GET(e4_sparks_on_large_explosions)) return;

  // The burst routine reads its texture variant from the caller's outgoing
  // argument area (+92); use variant 0 (GenericExplosion).
  const uint32_t r1 = uint32_t(a0.r1);
  const uint32_t saved_arg = REX_LOAD_U32(r1 + 92);
  REX_STORE_U32(r1 + 92, 0);

  SavedArgs a = a0;
  g_in_hook = true;
  uint32_t seed = Hash32(FloatBits(a.f1) * 3u ^ FloatBits(a.f2));
  DrawSparks(ctx, base, a, seed, float(frame), float(std::max(a.f3, a.f4)) * 0.6f, colour);
  g_in_hook = false;

  REX_STORE_U32(r1 + 92, saved_arg);
  ctx.r1.u64 = a.r1;
}
