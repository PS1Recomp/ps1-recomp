#pragma once

#include <cstdint>
#include <string>

namespace ps1::input {
class MemoryCard;
}

// The memory card's file system: block 0 holds the header and 15 directory
// frames, blocks 1..15 hold file data, chained through the directory
// (psx-spx controllersandmemorycards.md, "Memory Card Data Format").
// Blocks are numbered 0..14 here, as the directory's next-block pointers are.
namespace ps1::bios::cardfs {

constexpr int kBlocks = 15;
constexpr uint32_t kBlockBytes = 8192;

/// BIOS filename match: "?" matches one character, "*" ends the comparison.
bool match(const std::string &pattern, const std::string &name);

/// First block of the next file at or after `fromBlock` whose name matches,
/// or -1.
int find(const input::MemoryCard &card, const std::string &pattern,
         int fromBlock = 0);

std::string name(const input::MemoryCard &card, int block);
uint32_t size(const input::MemoryCard &card, int block);

/// Allocates `blocks` free blocks for a new file; returns its first block or
/// -1 when the name is taken or the card is full.
int create(input::MemoryCard &card, const std::string &name, int blocks);

/// Marks every block of the file deleted (A1h/A2h/A3h).
void erase(input::MemoryCard &card, int firstBlock);

/// Card sector holding byte `offset` of the file, or -1 past its last block.
int sectorAt(const input::MemoryCard &card, int firstBlock, uint32_t offset);

} // namespace ps1::bios::cardfs
