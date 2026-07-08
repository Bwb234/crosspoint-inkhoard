#pragma once

#include <Logging.h>

#include <cstdint>
#include <string>

#include "ProgressFile.h"

namespace EpubReaderUtils {

// Reader progress, FreeInkBook locator model. `charStart` (chapter character
// offset) is layout-parameter independent — it restores exactly across font,
// margin, spacing, and orientation changes. When a position is known only as
// a fraction of the chapter (KOReader remote sync, legacy migration), it is
// carried as `fractionQ16` with charStart == kNoCharStart and resolved
// against totalChars() once the chapter's page cache is open.
struct Progress {
  uint16_t spineIndex = 0;
  uint32_t charStart = 0;
  uint32_t fractionQ16 = 0;  // chapter fraction in Q16, used when charStart == kNoCharStart
  bool valid = false;
};

constexpr uint32_t kNoCharStart = 0xFFFFFFFFu;

// progress.bin v2: 'F','2', u16 spine, u32 charStart, u32 fractionQ16 (12 B,
// little-endian). Legacy v1 files (4 or 6 B: u16 spine, u16 page[, u16
// pageCount]) migrate on read to a chapter fraction — a sentence-accurate
// landing that becomes exact the first time v2 progress is saved.
inline bool saveProgress(const std::string& cachePath, const uint16_t spineIndex, const uint32_t charStart,
                         const uint32_t fractionQ16 = 0) {
  uint8_t data[12];
  data[0] = 'F';
  data[1] = '2';
  data[2] = spineIndex & 0xFF;
  data[3] = (spineIndex >> 8) & 0xFF;
  for (int i = 0; i < 4; ++i) data[4 + i] = (charStart >> (8 * i)) & 0xFF;
  for (int i = 0; i < 4; ++i) data[8 + i] = (fractionQ16 >> (8 * i)) & 0xFF;
  if (!ProgressFile::writeAtomic(cachePath, data, sizeof(data))) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%u char=%u", spineIndex, static_cast<unsigned>(charStart));
  return true;
}

inline Progress loadProgress(const std::string& cachePath) {
  Progress p;
  HalFile f;
  if (!Storage.openFileForRead("ERS", cachePath + "/progress.bin", f)) {
    return p;
  }
  uint8_t data[12];
  const int n = f.read(data, sizeof(data));
  if (n == 12 && data[0] == 'F' && data[1] == '2') {
    p.spineIndex = data[2] | (data[3] << 8);
    p.charStart = 0;
    p.fractionQ16 = 0;
    for (int i = 0; i < 4; ++i) p.charStart |= static_cast<uint32_t>(data[4 + i]) << (8 * i);
    for (int i = 0; i < 4; ++i) p.fractionQ16 |= static_cast<uint32_t>(data[8 + i]) << (8 * i);
    p.valid = true;
    return p;
  }
  if (n == 4 || n == 6) {  // legacy (spine, page[, pageCount]) — migrate to a fraction
    p.spineIndex = data[0] | (data[1] << 8);
    const uint16_t page = data[2] | (data[3] << 8);
    const uint16_t pageCount = n == 6 ? (data[4] | (data[5] << 8)) : 0;
    p.charStart = kNoCharStart;
    p.fractionQ16 =
        (pageCount > 0 && page != UINT16_MAX && page < pageCount) ? (static_cast<uint32_t>(page) << 16) / pageCount : 0;
    p.valid = true;
    LOG_INF("ERS", "Migrated legacy progress: spine=%u page=%u/%u", p.spineIndex, page, pageCount);
  }
  return p;
}

}  // namespace EpubReaderUtils
