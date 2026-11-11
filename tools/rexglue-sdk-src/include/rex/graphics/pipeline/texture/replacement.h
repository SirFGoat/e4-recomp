/**
 * Texture dumping and high-resolution replacement.
 *
 * Every 2D texture the title loads is identified by a hash of its guest data (plus format and
 * size), so the identifier is stable between runs regardless of where the title places the
 * texture in memory.
 *
 *  - texture_dump=true writes each newly seen texture once to <texture_dir>/dump/ as
 *    <W>x<H>_<format>_<hash>.png, decoded the way the shader sees it (before the fetch swizzle).
 *  - texture_replace=true looks in <texture_dir>/replace/ (recursively) for any PNG whose name
 *    ends in _<hash>.png and uses it instead of the original. The replacement may be any size;
 *    shaders address textures with normalized coordinates, so a 4x larger image just samples
 *    with more detail.
 *
 * Backend-agnostic: the D3D12 and Vulkan texture caches map their host formats onto
 * HostPixelFormat and call into this module.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, texture_dump);
REXCVAR_DECLARE(bool, texture_replace);
REXCVAR_DECLARE(std::string, texture_dir);

namespace rex::graphics::texture_replacement {

// Host texture layouts that can be decoded for dumping. Channel semantics are what a shader
// would read from an SRV/image view of that format (before the guest swizzle).
enum class HostPixelFormat {
  kUnsupported,
  kR8G8B8A8,
  kB8G8R8A8,
  kBC1,
  kBC2,
  kBC3,
  kB5G6R5,
  kB5G5R5A1,
  kB4G4R4A4,
  // Single/two-channel: dumped as grey (v,v,v,255) / (r,g,0,255); a replacement only needs the
  // red (and green) channel to carry the values.
  kR8,
  kR8G8,
  kBC4,
  kBC5,
};

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4, rows tightly packed.
  // Optional precomputed mip levels 1..n (filled when replacements are preloaded).
  std::vector<Image> mips;
};

bool IsDumpEnabled();
bool IsReplaceEnabled();

// Identifier of the guest texture content.
uint64_t HashGuestTexture(const void* guest_data, size_t guest_size, uint32_t guest_format,
                          uint32_t width, uint32_t height);

// Returns the replacement image for the hash, decoding and caching it on first use, or nullptr.
std::shared_ptr<const Image> FindReplacement(uint64_t hash);

// Starts decoding every replacement PNG on background threads (once per run; no-op when
// replacement is off), so first use in game doesn't stall on decoding.
void StartPreload();

// Returns true the first time it's called for a hash in this run (and only if dumping is on and
// the file doesn't already exist), so each texture is dumped once.
bool ClaimDump(uint64_t hash, uint32_t width, uint32_t height, const char* format_name,
               std::string& path_out);

// Decodes a host-layout level (rows row_pitch bytes apart; for block-compressed formats a row
// is a row of 4x4 blocks) into RGBA8. Returns false for unsupported formats.
bool DecodeHostLevel(HostPixelFormat format, const uint8_t* data, uint32_t row_pitch,
                     uint32_t width, uint32_t height, Image& image_out);

bool WritePng(const std::string& path, const Image& image);

// Box-filtered mip chain without copying: level 0 is `base`, further levels come from
// base.mips when precomputed, otherwise they're generated into `scratch` (which must outlive
// the returned pointers). Stops at 1x1 or level_count.
std::vector<const Image*> MipLevels(const Image& base, uint32_t level_count,
                                    std::vector<Image>& scratch);

}  // namespace rex::graphics::texture_replacement
