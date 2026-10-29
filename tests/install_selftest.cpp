// Installing from a disc image: a small synthetic XDVDFS image with the five game files, a wrong
// game, a missing file, and something that isn't a disc image at all.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "launcher/install.h"

namespace fs = std::filesystem;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

struct File {
  std::string name;
  std::vector<char> data;
};

std::vector<char> Xex(uint32_t title) {
  std::vector<char> x(64, 0);
  memcpy(x.data(), "XEX2", 4);
  auto be = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) x[at + i] = char(v >> (24 - 8 * i));
  };
  be(20, 1);           // one optional header
  be(24, 0x00040006);  // execution info...
  be(28, 32);          // ...at offset 32
  be(32 + 12, title);
  return x;
}

// Root directory entries chained through their right links; file data from sector 48 on.
void WriteImage(const fs::path& path, const std::vector<File>& files) {
  const uint32_t sector = 2048, root_sector = 40;
  std::vector<char> image(48 * sector, 0);
  memcpy(&image[32 * sector], "MICROSOFT*XBOX*MEDIA", 20);
  std::vector<char> table;
  uint32_t next_sector = 48;
  for (size_t i = 0; i < files.size(); ++i) {
    size_t off = table.size();
    size_t len = 14 + files[i].name.size();
    size_t end = (off + len + 3) & ~size_t(3);
    table.resize(end, 0);
    uint16_t right = i + 1 < files.size() ? uint16_t(end / 4) : 0;
    uint32_t start = next_sector, size = uint32_t(files[i].data.size());
    memcpy(&table[off + 2], &right, 2);
    memcpy(&table[off + 4], &start, 4);
    memcpy(&table[off + 8], &size, 4);
    table[off + 12] = 0x20;
    table[off + 13] = char(files[i].name.size());
    memcpy(&table[off + 14], files[i].name.data(), files[i].name.size());
    image.resize(size_t(next_sector) * sector, 0);
    image.insert(image.end(), files[i].data.begin(), files[i].data.end());
    next_sector = uint32_t((image.size() + sector - 1) / sector);
  }
  uint32_t table_size = uint32_t(table.size());
  memcpy(&image[32 * sector + 20], &root_sector, 4);
  memcpy(&image[32 * sector + 24], &table_size, 4);
  memcpy(&image[root_sector * sector], table.data(), table.size());
  std::ofstream(path, std::ios::binary).write(image.data(), std::streamsize(image.size()));
}

std::vector<char> Read(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
  if (argc == 3) {  // install_selftest <real disc image> <dest>: install it for real
    InstallProgress p;
    bool ok = InstallFromDiscImage(argv[1], argv[2], p);
    std::printf("%s (%llu MB)\n", ok ? "installed" : p.error.c_str(), (unsigned long long)(p.done >> 20));
    return ok ? 0 : 1;
  }
  fs::path dir = fs::temp_directory_path() / "monopoly_install_selftest";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::vector<File> files = {{"$SystemUpdate", {}}, {"DATA.BIG", std::vector<char>(5000, 'd')},
                             {"default.xex", Xex(0x4541086F)}, {"movies.big", std::vector<char>(3000, 'm')},
                             {"movies2.big", std::vector<char>(10, '2')}, {"music.big", std::vector<char>(4097, 'u')}};
  WriteImage(dir / "good.iso", files);
  InstallProgress p;
  bool ok = InstallFromDiscImage(dir / "good.iso", dir / "game", p);
  if (!ok) std::fprintf(stderr, "%s\n", p.error.c_str());
  CHECK(ok && p.finished && p.done == p.total);
  CHECK(Read(dir / "game" / "data.big") == files[1].data);
  CHECK(Read(dir / "game" / "default.xex") == files[2].data);
  CHECK(Read(dir / "game" / "music.big") == files[5].data);
  CHECK(!fs::exists(dir / "game" / "music.big.part"));

  auto other = files;
  other[2].data = Xex(0x4D530001);
  WriteImage(dir / "other.iso", other);
  InstallProgress p2;
  CHECK(!InstallFromDiscImage(dir / "other.iso", dir / "game2", p2) && p2.error.find("different game") != std::string::npos);

  auto partial = files;
  partial.pop_back();
  WriteImage(dir / "partial.iso", partial);
  InstallProgress p3;
  CHECK(!InstallFromDiscImage(dir / "partial.iso", dir / "game3", p3) && p3.error.find("music.big") != std::string::npos);

  std::ofstream(dir / "junk.iso") << "not a disc";
  InstallProgress p4;
  CHECK(!InstallFromDiscImage(dir / "junk.iso", dir / "game4", p4) && p4.error.find("isn't an Xbox 360") != std::string::npos);

  fs::remove_all(dir);
  std::printf("install selftest passed\n");
  return 0;
}
