// e4 - per-sprite neon glow (see e4_glow.h).

#include "e4_glow.h"

#include "generated/default/e4_init.h"

#include <bit>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(e4_sprite_glow, true, "E4",
                    "Enlarge sprites that have a baked glow (needs texture_replace)");

namespace e4::glow {
namespace {

struct Scale {
  float x, y;
};
struct TextureInfo {
  uint32_t width, height;
  Scale scale;
};

std::mutex g_mutex;
bool g_config_loaded = false;
std::unordered_map<uint64_t, Scale> g_scale_by_hash;
std::unordered_map<uint32_t, TextureInfo> g_glow_textures;  // texture handle -> info
std::map<std::string, std::string> g_names_csv;             // hash -> csv row
bool g_enabled = false;

std::filesystem::path TextureDir() {
  std::string d = rex::cvar::GetFlagByName("texture_dir");
  return std::filesystem::path(std::u8string(d.begin(), d.end()));
}

void LoadConfigLocked() {
  if (g_config_loaded) return;
  g_config_loaded = true;
  // The glow PNGs live in replace/@glow, which the texture replacement only
  // uses when "glow" is in texture_variants; enlarge quads only in that case.
  std::string variants = "," + rex::cvar::GetFlagByName("texture_variants") + ",";
  g_enabled = REXCVAR_GET(e4_sprite_glow) &&
              rex::cvar::GetFlagByName("texture_replace") == "true" &&
              variants.find(",glow,") != std::string::npos;
  if (!g_enabled) return;
  std::ifstream in(TextureDir() / "replace" / "@glow" / "glow_scale.txt");
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string hash_hex;
    Scale s{};
    if (ss >> hash_hex >> s.x >> s.y && s.x >= 1.0f && s.y >= 1.0f && s.x < 4.0f && s.y < 4.0f) {
      g_scale_by_hash[std::stoull(hash_hex, nullptr, 16)] = s;
    }
  }
  REXLOG_INFO("e4 glow: {} glowing sprites configured", g_scale_by_hash.size());
}

void WriteNamesCsvLocked() {
  std::error_code ec;
  auto dir = TextureDir();
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return;
  std::ofstream out(dir / "texture_names.csv");
  out << "name,size,hash\n";
  for (auto& [hash, row] : g_names_csv) out << row << "\n";
}

float LoadF(uint8_t* base, uint32_t addr) { return std::bit_cast<float>(REX_LOAD_U32(addr)); }
void StoreF(uint8_t* base, uint32_t addr, float v) { REX_STORE_U32(addr, std::bit_cast<uint32_t>(v)); }

}  // namespace

void OnTextureLoaded(uint32_t texture, const std::string& name, uint32_t width, uint32_t height,
                     uint64_t hash) {
  if (!hash) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  LoadConfigLocked();
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hash);
  if (g_names_csv.emplace(hex, name + "," + std::to_string(width) + "x" +
                                   std::to_string(height) + "," + hex)
          .second) {
    WriteNamesCsvLocked();
  }
  auto it = g_scale_by_hash.find(hash);
  if (it != g_scale_by_hash.end()) {
    g_glow_textures[texture] = {width, height, it->second};
  }
}

bool BeginSprite(PPCContext& ctx, uint8_t* base, SpriteScaleUndo& undo) {
  if (!g_enabled) return false;
  TextureInfo info;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_glow_textures.find(ctx.r5.u32);
    if (it == g_glow_textures.end()) return false;
    info = it->second;
  }
  uint32_t pos = ctx.r6.u32, pivot = ctx.r8.u32, scale = ctx.r9.u32;
  if (!pos || !scale) return false;
  undo.pos_addr = pos;
  undo.scale_addr = scale;
  for (int i = 0; i < 2; ++i) {
    undo.pos[i] = REX_LOAD_U32(pos + i * 4);
    undo.scale[i] = REX_LOAD_U32(scale + i * 4);
  }
  float sx = std::bit_cast<float>(undo.scale[0]), sy = std::bit_cast<float>(undo.scale[1]);
  // A null pivot means the sprite is centred (measured on the title screen).
  float px = pivot ? LoadF(base, pivot) : 0.5f, py = pivot ? LoadF(base, pivot + 4) : 0.5f;
  // Keep the shape's centre where it was: the quad grows by (k - 1) * size, of
  // which (0.5 - pivot) has to be taken off the position.
  float grow_x = (info.scale.x - 1.0f) * sx * float(info.width);
  float grow_y = (info.scale.y - 1.0f) * sy * float(info.height);
  StoreF(base, pos, std::bit_cast<float>(undo.pos[0]) - (0.5f - px) * grow_x);
  StoreF(base, pos + 4, std::bit_cast<float>(undo.pos[1]) - (0.5f - py) * grow_y);
  StoreF(base, scale, sx * info.scale.x);
  StoreF(base, scale + 4, sy * info.scale.y);
  return true;
}

void EndSprite(uint8_t* base, const SpriteScaleUndo& undo) {
  for (int i = 0; i < 2; ++i) {
    REX_STORE_U32(undo.pos_addr + i * 4, undo.pos[i]);
    REX_STORE_U32(undo.scale_addr + i * 4, undo.scale[i]);
  }
}

}  // namespace e4::glow
