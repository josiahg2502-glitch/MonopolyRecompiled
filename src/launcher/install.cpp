#include "install.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
// Where the game partition starts: rebuilt image, XGD2 (redump), XGD2, XGD3, XGD1.
constexpr uint64_t kPartitions[] = {0, 0xFD90000, 0xFDA0000, 0x2080000, 0x18300000};
constexpr const char* kWanted[] = {"default.xex", "data.big", "movies.big", "movies2.big", "music.big"};
constexpr uint32_t kTitleId = 0x4541086F;  // Monopoly (USA, Europe)

struct Entry {
  std::string name;
  uint64_t offset = 0, size = 0;
};

std::string Lower(std::string s) {
  for (char& c : s) c = char(tolower(uint8_t(c)));
  return s;
}

bool ReadAt(std::ifstream& f, uint64_t pos, void* out, size_t size) {
  f.clear();
  f.seekg(std::streamoff(pos));
  f.read(static_cast<char*>(out), std::streamsize(size));
  return size_t(f.gcount()) == size;
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// The files in the image's root directory (a binary tree of entries, see tools/xiso.py).
bool RootFiles(std::ifstream& f, std::vector<Entry>* out, std::string* error) {
  uint64_t base = UINT64_MAX;
  for (uint64_t b : kPartitions) {
    char magic[20];
    if (ReadAt(f, b + 32 * kSector, magic, 20) && memcmp(magic, kMagic, 20) == 0) {
      base = b;
      break;
    }
  }
  if (base == UINT64_MAX) return *error = "That file isn't an Xbox 360 disc image.", false;
  uint8_t root[8];
  if (!ReadAt(f, base + 32 * kSector + 20, root, 8)) return *error = "The disc image is cut short.", false;
  uint32_t sector, size;
  memcpy(&sector, root, 4);
  memcpy(&size, root + 4, 4);
  if (size == 0 || size > (16u << 20)) return *error = "The disc image's file table is damaged.", false;
  std::vector<uint8_t> table(size);
  if (!ReadAt(f, base + sector * kSector, table.data(), size)) return *error = "The disc image is cut short.", false;

  std::vector<size_t> stack{0}, seen;
  while (!stack.empty()) {
    size_t off = stack.back();
    stack.pop_back();
    if (off + 14 > table.size() || std::find(seen.begin(), seen.end(), off) != seen.end()) continue;
    seen.push_back(off);
    uint16_t left, right;
    uint32_t start, length;
    memcpy(&left, &table[off], 2);
    memcpy(&right, &table[off + 2], 2);
    if (left == 0xFFFF) continue;  // sector padding
    memcpy(&start, &table[off + 4], 4);
    memcpy(&length, &table[off + 8], 4);
    uint8_t attr = table[off + 12], name_len = table[off + 13];
    if (off + 14 + name_len > table.size()) continue;
    if (!(attr & 0x10))  // not a directory
      out->push_back({std::string(reinterpret_cast<char*>(&table[off + 14]), name_len), base + start * kSector, length});
    if (left) stack.push_back(size_t(left) * 4);
    if (right) stack.push_back(size_t(right) * 4);
  }
  return true;
}

// The title ID from default.xex's execution info header (XEX2, big endian), or 0.
uint32_t XexTitleId(std::ifstream& f, const Entry& xex) {
  uint8_t head[24];
  if (xex.size < 24 || !ReadAt(f, xex.offset, head, 24) || memcmp(head, "XEX2", 4) != 0) return 0;
  uint32_t count = Be32(head + 20);
  for (uint32_t i = 0; i < count && i < 64; ++i) {
    uint8_t kv[8];
    if (!ReadAt(f, xex.offset + 24 + i * 8, kv, 8)) return 0;
    if (Be32(kv) != 0x00040006) continue;  // execution info
    uint8_t info[16];
    uint32_t at = Be32(kv + 4);
    return at + 16 <= xex.size && ReadAt(f, xex.offset + at, info, 16) ? Be32(info + 12) : 0;
  }
  return 0;
}

}  // namespace

bool InstallFromDiscImage(const fs::path& image, const fs::path& dest, InstallProgress& p) {
  auto fail = [&](std::string why) {
    std::lock_guard lock(p.mutex);
    p.error = std::move(why);
    p.finished = true;
    return false;
  };
  std::ifstream f(image, std::ios::binary);
  if (!f) return fail("Couldn't open the disc image.");
  std::vector<Entry> files;
  std::string error;
  if (!RootFiles(f, &files, &error)) return fail(error);

  std::vector<Entry> wanted;
  for (const char* name : kWanted) {
    auto it = std::find_if(files.begin(), files.end(), [&](auto& e) { return Lower(e.name) == name; });
    if (it == files.end()) return fail("That disc image isn't Monopoly for Xbox 360 (it has no " + std::string(name) + ").");
    wanted.push_back({name, it->offset, it->size});
  }
  if (uint32_t id = XexTitleId(f, wanted[0]); id != kTitleId) {
    char hex[16];
    snprintf(hex, sizeof(hex), "%08X", id);
    return fail("That disc image is a different game (title " + std::string(hex) + "); this port needs Monopoly (2008).");
  }

  uint64_t total = 0;
  for (auto& e : wanted) total += e.size;
  p.total = total;
  std::error_code ec;
  fs::create_directories(dest, ec);
  ULARGE_INTEGER free_bytes{};
  if (GetDiskFreeSpaceExW(dest.c_str(), &free_bytes, nullptr, nullptr) && free_bytes.QuadPart < total + (64u << 20))
    return fail("Not enough free space: the game needs " + std::to_string(total >> 20) + " MB.");

  std::vector<char> buf(4u << 20);
  for (auto& e : wanted) {
    {
      std::lock_guard lock(p.mutex);
      p.file = e.name;
    }
    fs::path tmp = dest / (e.name + ".part"), final_path = dest / e.name;
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) return fail("Couldn't write to " + dest.string() + ".");
      f.clear();
      f.seekg(std::streamoff(e.offset));
      for (uint64_t left = e.size; left;) {
        if (p.cancel) {
          out.close();
          fs::remove(tmp, ec);
          return fail("Cancelled.");
        }
        size_t n = size_t(std::min<uint64_t>(left, buf.size()));
        f.read(buf.data(), std::streamsize(n));
        if (size_t(f.gcount()) != n) {
          out.close();
          fs::remove(tmp, ec);
          return fail("The disc image is cut short (in " + e.name + ").");
        }
        out.write(buf.data(), std::streamsize(n));
        left -= n;
        p.done += n;
      }
      if (!out.flush()) {
        out.close();
        fs::remove(tmp, ec);
        return fail("Couldn't write " + e.name + " (is the disk full?).");
      }
    }
    fs::rename(tmp, final_path, ec);
    if (ec) return fail("Couldn't finish writing " + e.name + ".");
  }
  p.finished = true;
  return true;
}
