#include "runtime/cdrom/bin_reader.h"

namespace ps1::cdrom {

bool BinReader::open(const std::filesystem::path &binPath) {
  file_.open(binPath, std::ios::binary | std::ios::ate);
  if (!file_.is_open()) {
    return false;
  }

  auto size = file_.tellg();
  totalSectors_ = static_cast<uint32_t>(size / SECTOR_SIZE_RAW);

  return true;
}

std::optional<Sector> BinReader::readSector(uint32_t sectorAddress) {
  if (!file_.is_open() || sectorAddress >= totalSectors_) {
    return std::nullopt;
  }

  Sector sector;
  file_.seekg(sectorAddress * SECTOR_SIZE_RAW, std::ios::beg);
  if (!file_.read(reinterpret_cast<char *>(sector.raw), SECTOR_SIZE_RAW)) {
    return std::nullopt;
  }

  return sector;
}

uint32_t BinReader::getTotalSectors() const { return totalSectors_; }

} // namespace ps1::cdrom
