// e4_extract - extracts an Xbox 360 STFS package (CON/LIVE/PIRS) into a folder.
// Used by the E4 builder kit to turn the player's XBLA package into game files.
//
// Usage: e4_extract <package file> <output folder>
// Plain C++17, no dependencies.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Entry {
  int index = 0;
  std::string name;
  bool dir = false;
  bool consecutive = false;
  uint32_t blocks = 0;
  uint32_t start = 0;
  int16_t parent = -1;
  uint32_t size = 0;
};

class Stfs {
 public:
  explicit Stfs(std::vector<uint8_t> data) : d_(std::move(data)) {}

  bool Parse(std::string& error) {
    if (d_.size() < 0x1000) return Fail(error, "file too small");
    const std::string magic(reinterpret_cast<const char*>(d_.data()), 4);
    if (magic != "CON " && magic != "LIVE" && magic != "PIRS")
      return Fail(error, "not an Xbox 360 package (STFS)");
    header_size_ = Be32(0x340);
    title_id_ = Be32(0x360);
    const size_t vd = 0x379;
    block_sep_ = d_[vd + 2];
    ft_block_count_ = uint32_t(d_[vd + 3]) | (uint32_t(d_[vd + 4]) << 8);
    ft_block_num_ = Le24(vd + 5);
    sex_ = (~block_sep_) & 1;  // 0 = one hash table copy, 1 = two
    first_hash_table_ = (header_size_ + 0xFFF) & 0xFFFFF000u;
    return true;
  }

  uint32_t title_id() const { return title_id_; }

  bool Entries(std::vector<Entry>& out, std::string& error) {
    std::vector<uint8_t> raw;
    if (!ReadBlocks(ft_block_num_, ft_block_count_, false, raw)) return Fail(error, "bad file table");
    for (size_t i = 0; i + 0x40 <= raw.size(); i += 0x40) {
      const uint8_t* e = raw.data() + i;
      const uint8_t flags = e[0x28];
      const int name_len = flags & 0x3F;
      if (name_len == 0) continue;
      Entry en;
      en.index = int(i / 0x40);
      en.name.assign(reinterpret_cast<const char*>(e), size_t(name_len));
      en.dir = flags & 0x80;
      en.consecutive = flags & 0x40;
      en.blocks = uint32_t(e[0x29]) | (uint32_t(e[0x2A]) << 8) | (uint32_t(e[0x2B]) << 16);
      en.start = uint32_t(e[0x2F]) | (uint32_t(e[0x30]) << 8) | (uint32_t(e[0x31]) << 16);
      en.parent = int16_t((uint16_t(e[0x32]) << 8) | e[0x33]);
      en.size = (uint32_t(e[0x34]) << 24) | (uint32_t(e[0x35]) << 16) | (uint32_t(e[0x36]) << 8) |
                e[0x37];
      out.push_back(en);
    }
    return true;
  }

  bool ReadBlocks(uint32_t start, uint32_t count, bool consecutive, std::vector<uint8_t>& out) {
    uint32_t n = start;
    for (uint32_t i = 0; i < count; ++i) {
      const uint64_t a = BlockAddr(n);
      if (a + 0x1000 > d_.size()) return false;
      out.insert(out.end(), d_.begin() + a, d_.begin() + a + 0x1000);
      if (i + 1 < count) {
        if (consecutive) {
          ++n;
        } else if (!NextBlock(n, n)) {
          return false;
        }
      }
    }
    return true;
  }

 private:
  static bool Fail(std::string& error, const char* msg) {
    error = msg;
    return false;
  }
  uint32_t Be32(size_t o) const {
    return (uint32_t(d_[o]) << 24) | (uint32_t(d_[o + 1]) << 16) | (uint32_t(d_[o + 2]) << 8) |
           d_[o + 3];
  }
  uint32_t Le24(size_t o) const {
    return uint32_t(d_[o]) | (uint32_t(d_[o + 1]) << 8) | (uint32_t(d_[o + 2]) << 16);
  }
  uint64_t BackingBlock(uint64_t n) const {
    const uint64_t s = sex_;
    const uint64_t r = (((n + 0xAA) / 0xAA) << s) + n;
    if (n < 0xAA) return r;
    if (n < 0x70E4) return r + (((n + 0x70E4) / 0x70E4) << s);
    return (1ull << s) + r + (((n + 0x70E4) / 0x70E4) << s);
  }
  uint64_t BlockAddr(uint64_t n) const { return (BackingBlock(n) << 12) + first_hash_table_; }
  uint64_t Level0HashBlock(uint64_t n) const {
    const uint64_t s = sex_;
    const uint64_t step0 = s == 0 ? 0xAB : 0xAC;
    if (n < 0xAA) return 0;
    uint64_t num = (n / 0xAA) * step0;
    num += ((n / 0x70E4) + 1) << s;
    if (n / 0x70E4 == 0) return num;
    return num + (1ull << s);
  }
  bool NextBlock(uint32_t n, uint32_t& next) const {
    uint64_t addr = (Level0HashBlock(n) << 12) + first_hash_table_;
    if (sex_ == 1) addr += uint64_t(block_sep_ & 2) << 11;  // active copy of the hash table
    addr += uint64_t(n % 0xAA) * 0x18;
    if (addr + 0x18 > d_.size()) return false;
    next = (uint32_t(d_[addr + 0x15]) << 16) | (uint32_t(d_[addr + 0x16]) << 8) | d_[addr + 0x17];
    return true;
  }

  std::vector<uint8_t> d_;
  uint32_t header_size_ = 0, title_id_ = 0, ft_block_count_ = 0, ft_block_num_ = 0;
  uint32_t first_hash_table_ = 0;
  uint8_t block_sep_ = 0;
  int sex_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: e4_extract <package file> <output folder>\n");
    return 2;
  }
  std::ifstream in(fs::u8path(argv[1]), std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "can't open %s\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Stfs s(std::move(data));
  std::string error;
  std::vector<Entry> entries;
  if (!s.Parse(error) || !s.Entries(entries, error)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  std::printf("Title %08X, %zu entries\n", s.title_id(), entries.size());

  std::map<int, const Entry*> by_index;
  for (const Entry& e : entries) by_index[e.index] = &e;
  auto full_path = [&](const Entry& e) {
    std::vector<std::string> parts{e.name};
    int p = e.parent;
    for (int guard = 0; p != -1 && by_index.count(p) && guard < 64; ++guard) {
      parts.push_back(by_index[p]->name);
      p = by_index[p]->parent;
    }
    fs::path out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out /= fs::u8path(*it);
    return out;
  };

  const fs::path root = fs::u8path(argv[2]);
  for (const Entry& e : entries) {
    if (e.name.find("..") != std::string::npos || e.name.find('/') != std::string::npos ||
        e.name.find('\\') != std::string::npos) {
      std::fprintf(stderr, "unsafe entry name in package: %s\n", e.name.c_str());
      return 1;
    }
    const fs::path dest = root / full_path(e);
    std::error_code ec;
    if (e.dir) {
      fs::create_directories(dest, ec);
      continue;
    }
    fs::create_directories(dest.parent_path(), ec);
    std::vector<uint8_t> bytes;
    if (!s.ReadBlocks(e.start, e.blocks, e.consecutive, bytes)) {
      std::fprintf(stderr, "damaged package: %s\n", e.name.c_str());
      return 1;
    }
    if (bytes.size() > e.size) bytes.resize(e.size);
    std::ofstream out(dest, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out) {
      std::fprintf(stderr, "can't write %s\n", dest.string().c_str());
      return 1;
    }
  }
  std::printf("Extracted to %s\n", argv[2]);
  return 0;
}
