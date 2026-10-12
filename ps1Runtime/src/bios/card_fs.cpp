#include "runtime/bios/card_fs.h"
#include "runtime/input/input.h"

#include <cstring>

namespace ps1::bios::cardfs {

namespace {

using input::MemoryCard;

// Directory frame of block b is card sector b+1; its data starts at (b+1)*64.
constexpr uint32_t kSectorsPerBlock = kBlockBytes / MemoryCard::SECTOR_SIZE;

struct Frame {
  uint8_t b[MemoryCard::SECTOR_SIZE];
  uint32_t state() const { return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24; }
  uint16_t next() const { return static_cast<uint16_t>(b[8] | b[9] << 8); }
};

Frame readFrame(const MemoryCard &card, int block) {
  Frame f;
  card.readSector(static_cast<uint32_t>(block + 1), f.b);
  return f;
}

void writeFrame(MemoryCard &card, int block, uint32_t state, uint32_t size,
                uint16_t next, const std::string &name) {
  uint8_t b[MemoryCard::SECTOR_SIZE] = {};
  std::memcpy(b, &state, 4);
  std::memcpy(b + 4, &size, 4);
  b[8] = static_cast<uint8_t>(next);
  b[9] = static_cast<uint8_t>(next >> 8);
  std::memcpy(b + 0x0A, name.c_str(), std::min<std::size_t>(name.size(), 20));
  for (int i = 0; i < 0x7F; ++i)
    b[0x7F] ^= b[i];
  card.writeSector(static_cast<uint32_t>(block + 1), b);
}

bool isFree(uint32_t state) { return (state & 0xF0) == 0xA0; }

} // namespace

bool match(const std::string &fullPattern, const std::string &name) {
  // The directory keeps 20 characters of a name, so only those can compare.
  const std::string pattern = fullPattern.substr(0, 20);
  for (std::size_t i = 0;; ++i) {
    const char p = i < pattern.size() ? pattern[i] : '\0';
    const char n = i < name.size() ? name[i] : '\0';
    if (p == '*')
      return true;
    if (p == '\0' || n == '\0')
      return p == n || (p == '?' && n == '\0'); // "?" may eat the terminator
    if (p != '?' && p != n)
      return false;
  }
}

int find(const MemoryCard &card, const std::string &pattern, int fromBlock) {
  for (int b = fromBlock; b < kBlocks; ++b)
    if (readFrame(card, b).state() == 0x51 && match(pattern, name(card, b)))
      return b;
  return -1;
}

std::string name(const MemoryCard &card, int block) {
  const Frame f = readFrame(card, block);
  return std::string(reinterpret_cast<const char *>(f.b + 0x0A),
                     strnlen(reinterpret_cast<const char *>(f.b + 0x0A), 21));
}

uint32_t size(const MemoryCard &card, int block) {
  const Frame f = readFrame(card, block);
  uint32_t s;
  std::memcpy(&s, f.b + 4, 4);
  return s;
}

int create(MemoryCard &card, const std::string &fileName, int blocks) {
  if (blocks < 1)
    blocks = 1;
  if (find(card, fileName) >= 0)
    return -1;
  int chain[kBlocks];
  int n = 0;
  for (int b = 0; b < kBlocks && n < blocks; ++b)
    if (isFree(readFrame(card, b).state()))
      chain[n++] = b;
  if (n < blocks)
    return -1;
  for (int i = 0; i < n; ++i) {
    const bool first = i == 0, last = i == n - 1;
    const uint32_t state = first ? 0x51 : last ? 0x53 : 0x52;
    const uint16_t next = last ? 0xFFFF : static_cast<uint16_t>(chain[i + 1]);
    writeFrame(card, chain[i], state, first ? n * kBlockBytes : 0, next,
               first ? fileName : std::string());
  }
  return chain[0];
}

void erase(MemoryCard &card, int firstBlock) {
  for (int b = firstBlock; b >= 0 && b < kBlocks;) {
    Frame f = readFrame(card, b);
    f.b[0] = static_cast<uint8_t>(0xA0 | (f.b[0] & 0x0F)); // 51h->A1h, ...
    f.b[0x7F] = 0;
    for (int i = 0; i < 0x7F; ++i)
      f.b[0x7F] ^= f.b[i];
    card.writeSector(static_cast<uint32_t>(b + 1), f.b);
    b = f.next() == 0xFFFF ? -1 : f.next();
  }
}

int sectorAt(const MemoryCard &card, int firstBlock, uint32_t offset) {
  int b = firstBlock;
  for (uint32_t skip = offset / kBlockBytes; skip > 0 && b >= 0; --skip) {
    const uint16_t next = readFrame(card, b).next();
    b = next < kBlocks ? next : -1;
  }
  if (b < 0)
    return -1;
  return static_cast<int>((b + 1) * kSectorsPerBlock +
                          (offset % kBlockBytes) / MemoryCard::SECTOR_SIZE);
}

} // namespace ps1::bios::cardfs
