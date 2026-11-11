/**
 * Texture dumping and high-resolution replacement - see replacement.h.
 */

#include <rex/graphics/pipeline/texture/replacement.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/logging.h>

// Private copies: stb symbols stay inside this plugin (rexruntime has its own stb_image).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_DEPRECATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

REXCVAR_DEFINE_BOOL(texture_dump, false, "GPU",
                    "Dump each texture the title loads once as PNG into <texture_dir>/dump");
REXCVAR_DEFINE_BOOL(texture_replace, true, "GPU",
                    "Replace textures with PNGs from <texture_dir>/replace whose name ends in "
                    "_<hash>.png (any resolution)");
REXCVAR_DEFINE_STRING(texture_dir, "textures", "GPU",
                      "Folder holding the 'dump' and 'replace' texture subfolders");
REXCVAR_DEFINE_STRING(texture_variants, "", "GPU",
                      "Comma-separated replacement variants to use: replace/**/@<variant>/ "
                      "folders are ignored unless listed, and override the plain PNGs when listed");

namespace rex::graphics::texture_replacement {

namespace fs = std::filesystem;

namespace {

struct Store {
  std::mutex mutex;
  bool indexed = false;
  std::unordered_map<uint64_t, fs::path> replacement_paths;
  std::unordered_map<uint64_t, std::shared_ptr<const Image>> replacement_images;
  std::unordered_set<uint64_t> failed;
  std::unordered_set<uint64_t> dumped;
};

Store& GetStore() {
  static Store store;
  return store;
}

fs::path RootDir() { return fs::u8path(REXCVAR_GET(texture_dir)); }

// Parses the trailing "_<16 hex digits>" of a file stem.
bool ParseHashFromStem(const std::string& stem, uint64_t& hash_out) {
  size_t underscore = stem.find_last_of('_');
  std::string hex = underscore == std::string::npos ? stem : stem.substr(underscore + 1);
  if (hex.size() != 16) {
    return false;
  }
  uint64_t value = 0;
  for (char c : hex) {
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return false;
    }
    value = (value << 4) | uint64_t(digit);
  }
  hash_out = value;
  return true;
}

void IndexReplacementsLocked(Store& store) {
  if (store.indexed) {
    return;
  }
  store.indexed = true;
  fs::path dir = RootDir() / "replace";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    REXGPU_INFO("Texture replacement: no folder at {}", dir.u8string());
    return;
  }
  // Folders named "@<variant>" hold optional alternatives: they're used only if
  // <variant> is listed in texture_variants, and then win over the plain PNGs.
  std::string variants = "," + REXCVAR_GET(texture_variants) + ",";
  std::unordered_map<uint64_t, fs::path> variant_paths;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::end(it);
       it.increment(ec)) {
    std::string folder = it->path().filename().u8string();
    if (it->is_directory(ec) && !folder.empty() && folder[0] == '@') {
      if (variants.find("," + folder.substr(1) + ",") == std::string::npos) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file(ec)) {
      continue;
    }
    std::string ext = it->path().extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    uint64_t hash;
    if (ext != ".png" || !ParseHashFromStem(it->path().stem().u8string(), hash)) {
      continue;
    }
    bool in_variant = false;
    for (fs::path p = it->path().parent_path(); p != dir && p.has_relative_path();
         p = p.parent_path()) {
      std::string name = p.filename().u8string();
      if (!name.empty() && name[0] == '@') {
        in_variant = true;
        break;
      }
    }
    (in_variant ? variant_paths : store.replacement_paths)[hash] = it->path();
  }
  for (auto& [hash, path] : variant_paths) {
    store.replacement_paths[hash] = path;
  }
  REXGPU_INFO("Texture replacement: {} replacement PNG(s) indexed in {}",
              store.replacement_paths.size(), dir.u8string());
}

uint8_t Expand(uint32_t value, uint32_t bits) {
  uint32_t max = (1u << bits) - 1;
  return uint8_t((value * 255 + max / 2) / max);
}

void DecodeRgb565(uint16_t c, uint8_t* rgb) {
  rgb[0] = Expand((c >> 11) & 31, 5);
  rgb[1] = Expand((c >> 5) & 63, 6);
  rgb[2] = Expand(c & 31, 5);
}

// Decodes the color part of a BC1/BC2/BC3 block into 16 RGBA texels (alpha left at 255).
void DecodeColorBlock(const uint8_t* block, bool allow_1bit_alpha, uint8_t out[16][4]) {
  uint16_t c0 = uint16_t(block[0] | (block[1] << 8));
  uint16_t c1 = uint16_t(block[2] | (block[3] << 8));
  uint8_t palette[4][4];
  DecodeRgb565(c0, palette[0]);
  DecodeRgb565(c1, palette[1]);
  palette[0][3] = palette[1][3] = 255;
  if (c0 > c1 || !allow_1bit_alpha) {
    for (int i = 0; i < 3; ++i) {
      palette[2][i] = uint8_t((2 * palette[0][i] + palette[1][i] + 1) / 3);
      palette[3][i] = uint8_t((palette[0][i] + 2 * palette[1][i] + 1) / 3);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) {
      palette[2][i] = uint8_t((palette[0][i] + palette[1][i]) / 2);
      palette[3][i] = 0;
    }
    palette[2][3] = 255;
    palette[3][3] = 0;
  }
  uint32_t indices = uint32_t(block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24));
  for (int i = 0; i < 16; ++i) {
    std::copy_n(palette[(indices >> (2 * i)) & 3], 4, out[i]);
  }
}

// 2x2 box filter to the next mip level.
Image Downsample(const Image& prev) {
  Image next;
  next.width = std::max(prev.width / 2, 1u);
  next.height = std::max(prev.height / 2, 1u);
  next.rgba.resize(size_t(next.width) * next.height * 4);
  for (uint32_t y = 0; y < next.height; ++y) {
    for (uint32_t x = 0; x < next.width; ++x) {
      uint32_t x0 = std::min(x * 2, prev.width - 1), x1 = std::min(x * 2 + 1, prev.width - 1);
      uint32_t y0 = std::min(y * 2, prev.height - 1), y1 = std::min(y * 2 + 1, prev.height - 1);
      const uint8_t* p00 = &prev.rgba[(size_t(y0) * prev.width + x0) * 4];
      const uint8_t* p01 = &prev.rgba[(size_t(y0) * prev.width + x1) * 4];
      const uint8_t* p10 = &prev.rgba[(size_t(y1) * prev.width + x0) * 4];
      const uint8_t* p11 = &prev.rgba[(size_t(y1) * prev.width + x1) * 4];
      uint8_t* dst = &next.rgba[(size_t(y) * next.width + x) * 4];
      for (int c = 0; c < 4; ++c) {
        dst[c] = uint8_t((p00[c] + p01[c] + p10[c] + p11[c] + 2) / 4);
      }
    }
  }
  return next;
}

// BC4-style 8-byte block (also BC3 alpha / each BC5 half) into one channel of 16 texels.
void DecodeBC4Channel(const uint8_t* block, uint8_t out[16][4], int channel);

void DecodeBC3Alpha(const uint8_t* block, uint8_t out[16][4]) {
  DecodeBC4Channel(block, out, 3);
}

void DecodeBC4Channel(const uint8_t* block, uint8_t out[16][4], int channel) {
  uint8_t a[8];
  a[0] = block[0];
  a[1] = block[1];
  if (a[0] > a[1]) {
    for (int i = 1; i < 7; ++i) {
      a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1] + 3) / 7);
    }
  } else {
    for (int i = 1; i < 5; ++i) {
      a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1] + 2) / 5);
    }
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) {
    bits |= uint64_t(block[2 + i]) << (8 * i);
  }
  for (int i = 0; i < 16; ++i) {
    out[i][channel] = a[(bits >> (3 * i)) & 7];
  }
}

}  // namespace

bool IsDumpEnabled() { return REXCVAR_GET(texture_dump); }
bool IsReplaceEnabled() { return REXCVAR_GET(texture_replace); }

uint64_t HashGuestTexture(const void* guest_data, size_t guest_size, uint32_t guest_format,
                          uint32_t width, uint32_t height) {
  uint64_t seed = (uint64_t(guest_format) << 48) ^ (uint64_t(width) << 24) ^ uint64_t(height);
  return XXH3_64bits_withSeed(guest_data, guest_size, seed);
}

namespace {

// Decodes one replacement PNG (no store lock held) and records the result in the store.
std::shared_ptr<const Image> DecodeAndCache(Store& store, uint64_t hash, const fs::path& file,
                                            bool log_each) {
  int w = 0, h = 0, channels = 0;
  std::string path = file.u8string();
  stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
  std::shared_ptr<Image> image;
  if (pixels) {
    image = std::make_shared<Image>();
    image->width = uint32_t(w);
    image->height = uint32_t(h);
    image->rgba.assign(pixels, pixels + size_t(w) * size_t(h) * 4);
    stbi_image_free(pixels);
    if (!log_each) {
      // Preloading: build the whole mip chain now too, off the GPU thread.
      const Image* prev = image.get();
      image->mips.reserve(32);
      while (prev->width > 1 || prev->height > 1) {
        image->mips.push_back(Downsample(*prev));
        prev = &image->mips.back();
      }
    }
  }
  std::lock_guard<std::mutex> lock(store.mutex);
  if (!image) {
    REXGPU_ERROR("Texture replacement: failed to decode {}", path);
    store.failed.insert(hash);
    return nullptr;
  }
  if (log_each) {
    REXGPU_INFO("Texture replacement: loaded {} ({}x{}) on demand", path, w, h);
  }
  // Another thread may have decoded it meanwhile; keep the first.
  return store.replacement_images.emplace(hash, std::move(image)).first->second;
}

}  // namespace

std::shared_ptr<const Image> FindReplacement(uint64_t hash) {
  Store& store = GetStore();
  fs::path file;
  {
    std::lock_guard<std::mutex> lock(store.mutex);
    IndexReplacementsLocked(store);
    auto cached = store.replacement_images.find(hash);
    if (cached != store.replacement_images.end()) {
      return cached->second;
    }
    auto path_it = store.replacement_paths.find(hash);
    if (path_it == store.replacement_paths.end() || store.failed.count(hash)) {
      return nullptr;
    }
    file = path_it->second;
  }
  // Not preloaded yet: decode it here (the lock is free meanwhile).
  return DecodeAndCache(store, hash, file, true);
}

void StartPreload() {
  if (!IsReplaceEnabled()) {
    return;
  }
  static std::once_flag once;
  std::call_once(once, [] {
    Store& store = GetStore();
    std::vector<std::pair<uint64_t, fs::path>> work;
    {
      std::lock_guard<std::mutex> lock(store.mutex);
      IndexReplacementsLocked(store);
      work.assign(store.replacement_paths.begin(), store.replacement_paths.end());
    }
    if (work.empty()) {
      return;
    }
    // Decode every replacement up front on background threads, so a texture's first
    // appearance in game doesn't stall the GPU thread on PNG decoding.
    std::thread([work = std::move(work)]() {
      Store& store = GetStore();
      const auto start = std::chrono::steady_clock::now();
      std::atomic<size_t> next{0}, done{0};
      const unsigned threads =
          std::clamp<unsigned>(std::thread::hardware_concurrency() / 2, 1, 8);
      std::vector<std::thread> pool;
      for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
          for (size_t i; (i = next.fetch_add(1)) < work.size();) {
            {
              std::lock_guard<std::mutex> lock(store.mutex);
              if (store.replacement_images.count(work[i].first) ||
                  store.failed.count(work[i].first)) {
                continue;
              }
            }
            if (DecodeAndCache(store, work[i].first, work[i].second, false)) {
              done.fetch_add(1);
            }
          }
        });
      }
      for (auto& thread : pool) {
        thread.join();
      }
      size_t bytes = 0;
      {
        std::lock_guard<std::mutex> lock(store.mutex);
        for (const auto& [hash, image] : store.replacement_images) {
          bytes += image->rgba.size();
          for (const auto& mip : image->mips) {
            bytes += mip.rgba.size();
          }
        }
      }
      REXGPU_INFO("Texture replacement: preloaded {} of {} PNG(s) in {:.2f} s ({} MB decoded)",
                  done.load(), work.size(),
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                  bytes >> 20);
    }).detach();
  });
}

bool ClaimDump(uint64_t hash, uint32_t width, uint32_t height, const char* format_name,
               std::string& path_out) {
  if (!IsDumpEnabled()) {
    return false;
  }
  Store& store = GetStore();
  std::lock_guard<std::mutex> lock(store.mutex);
  if (!store.dumped.insert(hash).second) {
    return false;
  }
  fs::path dir = RootDir() / "dump";
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path path = dir / fmt::format("{}x{}_{}_{:016x}.png", width, height, format_name, hash);
  if (fs::exists(path, ec)) {
    return false;
  }
  path_out = path.u8string();
  return true;
}

bool DecodeHostLevel(HostPixelFormat format, const uint8_t* data, uint32_t row_pitch,
                     uint32_t width, uint32_t height, Image& image_out) {
  image_out.width = width;
  image_out.height = height;
  image_out.rgba.assign(size_t(width) * height * 4, 0);
  uint8_t* out = image_out.rgba.data();
  switch (format) {
    case HostPixelFormat::kR8G8B8A8:
    case HostPixelFormat::kB8G8R8A8:
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = data + size_t(y) * row_pitch;
        uint8_t* dst = out + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x, src += 4, dst += 4) {
          bool bgra = format == HostPixelFormat::kB8G8R8A8;
          dst[0] = src[bgra ? 2 : 0];
          dst[1] = src[1];
          dst[2] = src[bgra ? 0 : 2];
          dst[3] = src[3];
        }
      }
      return true;
    case HostPixelFormat::kB5G6R5:
    case HostPixelFormat::kB5G5R5A1:
    case HostPixelFormat::kB4G4R4A4:
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = data + size_t(y) * row_pitch;
        uint8_t* dst = out + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x, src += 2, dst += 4) {
          uint16_t v = uint16_t(src[0] | (src[1] << 8));
          if (format == HostPixelFormat::kB5G6R5) {
            DecodeRgb565(v, dst);
            dst[3] = 255;
          } else if (format == HostPixelFormat::kB5G5R5A1) {
            dst[0] = Expand((v >> 10) & 31, 5);
            dst[1] = Expand((v >> 5) & 31, 5);
            dst[2] = Expand(v & 31, 5);
            dst[3] = (v >> 15) ? 255 : 0;
          } else {
            dst[0] = Expand((v >> 8) & 15, 4);
            dst[1] = Expand((v >> 4) & 15, 4);
            dst[2] = Expand(v & 15, 4);
            dst[3] = Expand((v >> 12) & 15, 4);
          }
        }
      }
      return true;
    case HostPixelFormat::kR8:
    case HostPixelFormat::kR8G8:
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = data + size_t(y) * row_pitch;
        uint8_t* dst = out + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x, dst += 4) {
          if (format == HostPixelFormat::kR8) {
            dst[0] = dst[1] = dst[2] = src[x];
          } else {
            dst[0] = src[x * 2];
            dst[1] = src[x * 2 + 1];
            dst[2] = 0;
          }
          dst[3] = 255;
        }
      }
      return true;
    case HostPixelFormat::kBC4:
    case HostPixelFormat::kBC5: {
      uint32_t block_bytes = format == HostPixelFormat::kBC4 ? 8 : 16;
      uint32_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
      for (uint32_t by = 0; by < blocks_y; ++by) {
        const uint8_t* row = data + size_t(by) * row_pitch;
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
          const uint8_t* block = row + size_t(bx) * block_bytes;
          uint8_t texels[16][4] = {};
          DecodeBC4Channel(block, texels, 0);
          if (format == HostPixelFormat::kBC5) {
            DecodeBC4Channel(block + 8, texels, 1);
          }
          for (uint32_t i = 0; i < 16; ++i) {
            if (format == HostPixelFormat::kBC4) {
              texels[i][1] = texels[i][2] = texels[i][0];
            }
            texels[i][3] = 255;
            uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
            if (x < width && y < height) {
              std::copy_n(texels[i], 4, out + (size_t(y) * width + x) * 4);
            }
          }
        }
      }
      return true;
    }
    case HostPixelFormat::kBC1:
    case HostPixelFormat::kBC2:
    case HostPixelFormat::kBC3: {
      uint32_t block_bytes = format == HostPixelFormat::kBC1 ? 8 : 16;
      uint32_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
      for (uint32_t by = 0; by < blocks_y; ++by) {
        const uint8_t* row = data + size_t(by) * row_pitch;
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
          const uint8_t* block = row + size_t(bx) * block_bytes;
          uint8_t texels[16][4];
          if (format == HostPixelFormat::kBC1) {
            DecodeColorBlock(block, true, texels);
          } else {
            DecodeColorBlock(block + 8, false, texels);
            if (format == HostPixelFormat::kBC2) {
              for (int i = 0; i < 16; ++i) {
                texels[i][3] = Expand((block[i / 2] >> (4 * (i & 1))) & 15, 4);
              }
            } else {
              DecodeBC3Alpha(block, texels);
            }
          }
          for (uint32_t i = 0; i < 16; ++i) {
            uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
            if (x < width && y < height) {
              std::copy_n(texels[i], 4, out + (size_t(y) * width + x) * 4);
            }
          }
        }
      }
      return true;
    }
    default:
      return false;
  }
}

bool WritePng(const std::string& path, const Image& image) {
  if (!stbi_write_png(path.c_str(), int(image.width), int(image.height), 4, image.rgba.data(),
                      int(image.width * 4))) {
    REXGPU_ERROR("Texture dump: failed to write {}", path);
    return false;
  }
  REXGPU_INFO("Texture dump: wrote {}", path);
  return true;
}

std::vector<const Image*> MipLevels(const Image& base, uint32_t level_count,
                                    std::vector<Image>& scratch) {
  std::vector<const Image*> levels{&base};
  // Every level is added before use, so reserve to keep the scratch pointers stable.
  scratch.clear();
  scratch.reserve(32);
  while (levels.size() < level_count) {
    const Image& prev = *levels.back();
    if (prev.width == 1 && prev.height == 1) {
      break;
    }
    size_t index = levels.size() - 1;  // into base.mips
    if (index < base.mips.size()) {
      levels.push_back(&base.mips[index]);
    } else {
      scratch.push_back(Downsample(prev));
      levels.push_back(&scratch.back());
    }
  }
  return levels;
}

}  // namespace rex::graphics::texture_replacement
