#include "FreeInkPageRenderer.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <render/ImageRenderer.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "BookPaginator.h"
#include "CrossPointSettings.h"

using freeink::book::Page;
using freeink::book::PageImage;
using freeink::book::PageTextRun;

namespace {

// Decode scratch for one image: inflate window + PNG/JPEG decoder state.
// Ideal covers a deflated PNG (46 KB inflate + decoder + row buffers); the
// size flexes down to what the largest free block can give — a stored JPEG
// needs far less, and ImageRenderer fails soft (OutOfMemory) if the arena
// really is too small for the specific image.
constexpr size_t kImageScratchIdeal = 72 * 1024;
constexpr size_t kImageScratchFloor = 32 * 1024;
constexpr size_t kImageAllocSlack = 64;

// 4x4 Bayer matrix (0..15) for quantizing the two mid-gray levels in BW mode.
constexpr uint8_t kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

EpdFontFamily::Style styleFor(const uint8_t flags) {
  // Only the face-selecting bits: the engine pre-shifts sub/sup baselines and
  // pre-sizes their runs, and underline is drawn as a rect below.
  uint8_t s = EpdFontFamily::REGULAR;
  if (flags & freeink::book::StyleBold) s |= EpdFontFamily::BOLD;
  if (flags & freeink::book::StyleItalic) s |= EpdFontFamily::ITALIC;
  return static_cast<EpdFontFamily::Style>(s);
}

// --- 2-bit image cache -----------------------------------------------------
//
// File: u16 width, u16 height, then rows packed 4 pixels/byte, MSB-first,
// values in screen convention (0 = black .. 3 = white).

void imageCachePath(const std::string& cacheDir, const PageImage& img, char* out, const size_t outCap) {
  snprintf(out, outCap, "%s/i%08x_%ux%u.g2", cacheDir.c_str(), freeink::book::ZipCatalog::hashPath(img.href), img.width,
           img.height);
}

struct G2Writer {
  HalFile file;
  uint8_t rowBuf[512];  // packed row, up to 2048 px wide
  uint16_t width = 0;
  bool failed = false;

  static bool onRow(void* user, const uint16_t y, const uint8_t* gray, const uint16_t width) {
    (void)y;
    auto* self = static_cast<G2Writer*>(user);
    if (self->failed || width != self->width || (width + 3u) / 4u > sizeof(self->rowBuf)) {
      self->failed = true;
      return false;
    }
    const uint16_t rowBytes = (width + 3) / 4;
    memset(self->rowBuf, 0, rowBytes);
    for (uint16_t i = 0; i < width; ++i) {
      const uint8_t level = gray[i] >> 6;  // 0=black .. 3=white
      self->rowBuf[i >> 2] |= level << ((3 - (i & 3)) * 2);
    }
    if (self->file.write(self->rowBuf, rowBytes) != rowBytes) self->failed = true;
    return !self->failed;
  }
};

bool ensureImageCached(BookPaginator& paginator, const std::string& cacheDir, const PageImage& img, const char* path) {
  if (Storage.exists(path)) return true;
  (void)cacheDir;

  const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t scratchSize = std::min(kImageScratchIdeal, block > kImageAllocSlack ? block - kImageAllocSlack : 0);
  std::unique_ptr<uint8_t[]> scratchBuf;
  if (scratchSize >= kImageScratchFloor) scratchBuf = makeUniqueNoThrow<uint8_t[]>(scratchSize);
  if (!scratchBuf) {
    LOG_ERR("FIBIMG", "OOM: image decode scratch (want %u B, max block %u)", static_cast<unsigned>(scratchSize),
            static_cast<unsigned>(block));
    return false;
  }
  freeink::book::Arena scratch(scratchBuf.get(), scratchSize);

  char tmpPath[192];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  G2Writer writer;
  writer.width = img.width;
  if (!Storage.openFileForWrite("FIBIMG", tmpPath, writer.file)) return false;
  const uint8_t header[4] = {static_cast<uint8_t>(img.width & 0xFF), static_cast<uint8_t>(img.width >> 8),
                             static_cast<uint8_t>(img.height & 0xFF), static_cast<uint8_t>(img.height >> 8)};
  writer.failed = writer.file.write(header, sizeof(header)) != sizeof(header);

  const freeink::book::BookStatus st = freeink::book::ImageRenderer::render(*paginator.bookSource(), paginator.zip(),
                                                                            img, scratch, &G2Writer::onRow, &writer);
  writer.file.close();  // close before remove/rename below
  if (st != freeink::book::BookStatus::Ok || writer.failed) {
    LOG_ERR("FIBIMG", "Image decode failed (%d): %s", static_cast<int>(st), img.href);
    Storage.remove(tmpPath);
    return false;
  }
  Storage.remove(path);  // may not exist
  if (!Storage.rename(tmpPath, path)) {
    Storage.remove(tmpPath);
    return false;
  }
  return true;
}

void drawImageFromCache(const GfxRenderer& renderer, const char* path, const PageImage& img) {
  HalFile f;
  if (!Storage.openFileForRead("FIBIMG", path, f)) return;
  uint8_t header[4];
  if (f.read(header, 4) != 4) return;
  const uint16_t w = header[0] | (header[1] << 8);
  const uint16_t h = header[2] | (header[3] << 8);
  if (w != img.width || h != img.height || w == 0) return;

  const GfxRenderer::RenderMode mode = renderer.getRenderMode();
  uint8_t rowBuf[512];
  const uint16_t rowBytes = (w + 3) / 4;
  if (rowBytes > sizeof(rowBuf)) return;

  for (uint16_t y = 0; y < h; ++y) {
    const int screenY = img.y + y;
    // Tiled-grayscale band culling: rows outside the active strip are seeked
    // past, not read — without this every strip pass re-reads and re-iterates
    // the whole image (12+ full passes for a cover page, seconds of work).
    if (!renderer.glyphIntersectsStrip(img.x, screenY, img.x + w, screenY + 1)) {
      if (!f.seekCur(rowBytes)) return;
      continue;
    }
    if (f.read(rowBuf, rowBytes) != rowBytes) return;
    for (uint16_t x = 0; x < w; ++x) {
      const uint8_t level = (rowBuf[x >> 2] >> ((3 - (x & 3)) * 2)) & 0x3;  // 0=black..3=white
      const int screenX = img.x + x;
      if (mode == GfxRenderer::BW) {
        // Solid black/white plus Bayer dithering for the two gray levels, so
        // panels without a grayscale pass still show shading.
        const bool black = level == 0 || (level < 3 && (level * 5) <= kBayer4[y & 3][x & 3]);
        if (black) renderer.drawPixel(screenX, screenY, true);
      } else if (mode == GfxRenderer::GRAYSCALE_MSB) {
        // Same plane convention as 2-bit glyphs: mark grays with state=false.
        if (level == 1 || level == 2) renderer.drawPixel(screenX, screenY, false);
      } else if (mode == GfxRenderer::GRAYSCALE_LSB) {
        if (level == 1) renderer.drawPixel(screenX, screenY, false);
      }
    }
  }
}

}  // namespace

namespace FreeInkPageRenderer {

void drawPage(GfxRenderer& renderer, BookPaginator& paginator, const Page& page, const std::string& cacheDir) {
  char textBuf[512];

  for (uint16_t r = 0; r < page.runCount; ++r) {
    const PageTextRun& run = page.runs[r];
    const uint16_t len = run.len < sizeof(textBuf) - 1 ? run.len : sizeof(textBuf) - 1;
    memcpy(textBuf, run.text, len);
    textBuf[len] = '\0';

    const int fontId = paginator.fontIdForRunSize(run.sizePx);
    const EpdFontFamily::Style style = styleFor(run.styleFlags);
    // drawText's y is the line top; runs carry the baseline. NONE: the engine
    // already produced visual order — never reorder here.
    const int top = run.baselineY - renderer.getFontAscenderSize(fontId);
    renderer.drawText(fontId, run.x, top, textBuf, true, style, BidiUtils::BidiBaseDir::NONE);

    if (run.styleFlags & freeink::book::StyleUnderline) {
      const int width = renderer.getTextWidth(fontId, textBuf, style, BidiUtils::BidiBaseDir::NONE);
      renderer.drawLine(run.x, run.baselineY + 2, run.x + width - 1, run.baselineY + 2, true);
    }
  }

  auto* fcm = renderer.getFontCacheManager();
  const bool scanning = fcm != nullptr && fcm->isScanning();
  if (scanning || SETTINGS.imageRendering == CrossPointSettings::IMAGES_SUPPRESS) return;
  if (SETTINGS.imageRendering == CrossPointSettings::IMAGES_PLACEHOLDER) {
    if (renderer.getRenderMode() == GfxRenderer::BW) {
      for (uint16_t m = 0; m < page.imageCount; ++m) {
        const PageImage& img = page.images[m];
        renderer.drawRect(img.x, img.y, img.width, img.height, true);
      }
    }
    return;
  }

  for (uint16_t m = 0; m < page.imageCount; ++m) {
    const PageImage& img = page.images[m];
    char path[192];
    imageCachePath(cacheDir, img, path, sizeof(path));
    if (ensureImageCached(paginator, cacheDir, img, path)) {
      drawImageFromCache(renderer, path, img);
    }
  }
}

bool imageBoundingBox(const Page& page, int16_t* x, int16_t* y, int16_t* w, int16_t* h) {
  if (page.imageCount == 0) return false;
  int16_t minX = INT16_MAX, minY = INT16_MAX, maxX = INT16_MIN, maxY = INT16_MIN;
  for (uint16_t m = 0; m < page.imageCount; ++m) {
    const PageImage& img = page.images[m];
    minX = std::min(minX, img.x);
    minY = std::min(minY, img.y);
    maxX = std::max<int16_t>(maxX, img.x + img.width);
    maxY = std::max<int16_t>(maxY, img.y + img.height);
  }
  *x = minX;
  *y = minY;
  *w = maxX - minX;
  *h = maxY - minY;
  return true;
}

std::vector<FootnoteEntry> collectFootnotes(const Page& page) {
  std::vector<FootnoteEntry> notes;
  notes.reserve(page.linkCount);
  for (uint16_t l = 0; l < page.linkCount; ++l) {
    const freeink::book::PageLink& link = page.links[l];
    FootnoteEntry entry;
    if (link.fragment != nullptr && link.fragment[0] != '\0') {
      snprintf(entry.href, sizeof(entry.href), "%s#%s", link.target != nullptr ? link.target : "", link.fragment);
    } else if (link.target != nullptr && link.target[0] != '\0') {
      snprintf(entry.href, sizeof(entry.href), "%s", link.target);
    } else {
      continue;
    }
    snprintf(entry.number, sizeof(entry.number), "%u", static_cast<unsigned>(notes.size() + 1));
    notes.push_back(entry);
  }
  return notes;
}

std::string pageText(const Page& page) {
  std::string text;
  size_t total = 0;
  for (uint16_t r = 0; r < page.runCount; ++r) total += page.runs[r].len + 1;
  text.reserve(total);
  for (uint16_t r = 0; r < page.runCount; ++r) {
    if (r > 0) text += ' ';  // runs carry no separators (justified gaps are positional)
    text.append(page.runs[r].text, page.runs[r].len);
  }
  return text;
}

}  // namespace FreeInkPageRenderer
