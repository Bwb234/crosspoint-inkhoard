#include "BookCoverUtils.h"

#include <FreeInkBook.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "activities/reader/EpubReaderUtils.h"     // cacheDirForBook
#include "activities/reader/FreeInkBookStorage.h"  // SdBookSource

namespace {

using EpubReaderUtils::cacheDirForBook;
using freeink::book::Arena;
using freeink::book::Book;
using freeink::book::BookStatus;
using freeink::book::ManifestItem;
using freeink::book::ZipEntryReader;

// Container open is transient: metadata + ZIP catalog only, freed on return.
// Cover work runs at the home screen after its render has already carved up
// the heap, so the fixed-size scratch (measured container-parse high-water is
// ~47 KB) allocates first and the book arena flexes to whatever block
// remains — corpus book footprints are 5-40 KB, so a shrunken arena still
// opens almost everything, and a too-big book fails soft (retried next visit).
constexpr size_t kScratchSize = 52 * 1024;
constexpr size_t kBookArenaIdeal = 48 * 1024;
constexpr size_t kBookArenaFloor = 20 * 1024;
constexpr size_t kAllocSlack = 64;  // TLSF block header/split overhead

// Opens the book file's container long enough to run `fn(book, source)`.
template <typename Fn>
bool withOpenBook(const std::string& epubPath, Fn&& fn) {
  auto scratchBuf = makeUniqueNoThrow<uint8_t[]>(kScratchSize);
  std::unique_ptr<uint8_t[]> bookBuf;
  size_t bookSize = 0;
  if (scratchBuf) {
    const size_t blockNow = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    bookSize = std::min(kBookArenaIdeal, blockNow > kAllocSlack ? blockNow - kAllocSlack : 0);
    if (bookSize >= kBookArenaFloor) bookBuf = makeUniqueNoThrow<uint8_t[]>(bookSize);
  }
  if (!bookBuf || !scratchBuf) {
    LOG_ERR("COVER", "OOM: book open arenas (book %u B, free heap %u, max block %u)", static_cast<unsigned>(bookSize),
            static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return false;
  }
  Arena bookArena(bookBuf.get(), bookSize);
  Arena scratch(scratchBuf.get(), kScratchSize);

  SdBookSource source;
  if (!source.open(epubPath.c_str())) {
    LOG_ERR("COVER", "Cannot open: %s", epubPath.c_str());
    return false;
  }
  Book book;
  const BookStatus st = book.open(source, bookArena, scratch);
  if (st != BookStatus::Ok) {
    LOG_ERR("COVER", "Book open failed: %d (%s)", static_cast<int>(st), epubPath.c_str());
    return false;
  }
  return fn(book, source, scratch);
}

const ManifestItem* findCoverItem(const Book& book) {
  for (size_t m = 0; m < book.manifestCount(); ++m) {
    const ManifestItem* item = book.manifestItem(m);
    if (item != nullptr && item->isCoverImage) return item;
  }
  return nullptr;
}

bool isJpegItem(const ManifestItem& item) {
  if (item.mediaType != nullptr && strcmp(item.mediaType, "image/jpeg") == 0) return true;
  return FsHelpers::hasJpgExtension(std::string_view(item.href));
}

bool isPngItem(const ManifestItem& item) {
  if (item.mediaType != nullptr && strcmp(item.mediaType, "image/png") == 0) return true;
  return FsHelpers::hasPngExtension(std::string_view(item.href));
}

// Streams the cover entry out of the ZIP into `tempPath`.
bool extractItem(const Book& book, freeink::book::BookSource& source, Arena& scratch, const ManifestItem& item,
                 const std::string& tempPath) {
  const freeink::book::ZipEntry* entry = book.zip().find(item.href);
  if (entry == nullptr) {
    LOG_ERR("COVER", "Cover entry missing: %s", item.href);
    return false;
  }

  const size_t marked = scratch.mark();
  ZipEntryReader reader;
  uint8_t* buf = static_cast<uint8_t*>(scratch.alloc(4096, 1));
  if (buf == nullptr || reader.open(source, *entry, scratch) != BookStatus::Ok) {
    scratch.release(marked);
    return false;
  }

  bool ok = true;
  {
    HalFile out;
    if (!Storage.openFileForWrite("COVER", tempPath, out)) {
      scratch.release(marked);
      return false;
    }
    for (;;) {
      const int32_t n = reader.read(buf, 4096);
      if (n < 0) {
        ok = false;
        break;
      }
      if (n == 0) break;
      if (out.write(buf, n) != static_cast<size_t>(n)) {
        ok = false;
        break;
      }
    }
    // `out` closes at scope exit, before the converter reopens the path.
  }
  scratch.release(marked);
  if (!ok) Storage.remove(tempPath.c_str());
  return ok;
}

// Shared shape of both generators: extract the cover beside the output, run
// `convert(coverFile, bmpOut)`, clean up the temp, drop the output on failure.
// `coverlessOut` is true only when the container OPENED and provably has no
// usable cover — the one case callers may cache negatively; every other
// failure is potentially transient (OOM, SD hiccup) and worth retrying.
//
// The conversion runs AFTER withOpenBook returns: extraction needs the book
// arenas (~95 KB), the JPEG/PNG decoder needs ~53 KB of its own, and the
// heap cannot hold both at once — the temp file is the handoff between them.
template <typename ConvertFn>
bool generateFromCover(const std::string& epubPath, const std::string& outPath, ConvertFn&& convert,
                       bool* coverlessOut) {
  if (coverlessOut != nullptr) *coverlessOut = false;
  const std::string cacheDir = cacheDirForBook(epubPath);
  Storage.ensureDirectoryExists(cacheDir.c_str());

  bool coverless = false;
  bool jpeg = false;
  std::string tempPath;
  const bool opened = withOpenBook(epubPath, [&](Book& book, freeink::book::BookSource& source, Arena& scratch) {
    const ManifestItem* cover = findCoverItem(book);
    if (cover == nullptr) {
      LOG_DBG("COVER", "No cover image in manifest: %s", epubPath.c_str());
      coverless = true;
      return true;  // opened fine, just coverless
    }
    jpeg = isJpegItem(*cover);
    const bool png = !jpeg && isPngItem(*cover);
    if (!jpeg && !png) {
      LOG_ERR("COVER", "Unsupported cover format: %s", cover->href);
      coverless = true;  // format will not improve on retry
      return true;
    }

    const std::string extractPath = cacheDir + (jpeg ? "/.cover.jpg" : "/.cover.png");
    if (extractItem(book, source, scratch, *cover, extractPath)) tempPath = extractPath;
    return true;
  });

  bool converted = false;
  if (opened && !tempPath.empty()) {
    {
      HalFile coverFile;
      HalFile bmpOut;
      if (Storage.openFileForRead("COVER", tempPath, coverFile) && Storage.openFileForWrite("COVER", outPath, bmpOut)) {
        converted = convert(jpeg, coverFile, bmpOut);
      }
      // Both close at scope exit, before the temp file is removed below.
    }
    Storage.remove(tempPath.c_str());
  }

  if (coverlessOut != nullptr) *coverlessOut = opened && coverless;
  if (!converted) Storage.remove(outPath.c_str());
  return opened && converted;
}

}  // namespace

namespace BookCoverUtils {

std::string coverBmpPath(const std::string& epubPath, const bool cropped) {
  return cacheDirForBook(epubPath) + (cropped ? "/cover_crop.bmp" : "/cover.bmp");
}

std::string thumbBmpPath(const std::string& epubPath, const int height) {
  return cacheDirForBook(epubPath) + "/thumb_" + std::to_string(height) + ".bmp";
}

std::string thumbBmpPathTemplate(const std::string& epubPath) {
  return cacheDirForBook(epubPath) + "/thumb_[HEIGHT].bmp";
}

bool generateCoverBmp(const std::string& epubPath, const bool cropped) {
  const std::string outPath = coverBmpPath(epubPath, cropped);
  if (Storage.exists(outPath.c_str())) return true;
  return generateFromCover(
      epubPath, outPath,
      [cropped](const bool jpeg, HalFile& coverFile, HalFile& bmpOut) {
        return jpeg ? JpegToBmpConverter::jpegFileToBmpStream(coverFile, bmpOut, cropped)
                    : PngToBmpConverter::pngFileToBmpStream(coverFile, bmpOut, cropped);
      },
      nullptr);
}

bool thumbAttemptNeeded(const std::string& thumbPath) {
  HalFile f;
  if (!Storage.openFileForRead("COVER", thumbPath, f)) return true;  // missing
  const size_t size = f.fileSize();
  // 0 = stale sentinel from a transient failure (legacy behavior wrote these
  // for ANY failure): retry. 1 = decided coverless: leave it. >1 = real BMP.
  return size == 0;
}

bool generateThumbBmp(const std::string& epubPath, const int height) {
  const std::string outPath = thumbBmpPath(epubPath, height);
  if (Storage.exists(outPath.c_str())) {
    if (!thumbAttemptNeeded(outPath)) return true;
    Storage.remove(outPath.c_str());
    LOG_INF("COVER", "Retrying thumbnail with stale sentinel: %s", outPath.c_str());
  }

  const int targetWidth = height * 6 / 10;  // Continue Reading card aspect (legacy)
  bool coverless = false;
  const bool ok = generateFromCover(
      epubPath, outPath,
      [targetWidth, height](const bool jpeg, HalFile& coverFile, HalFile& bmpOut) {
        return jpeg ? JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverFile, bmpOut, targetWidth, height)
                    : PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverFile, bmpOut, targetWidth, height);
      },
      &coverless);
  if (ok) return true;

  // Sentinel ONLY when the container opened and provably lacks a usable
  // cover — one byte, so it is distinguishable from the zero-byte leftovers
  // of transient failures. Everything else leaves nothing behind and the
  // next home screen visit retries.
  if (coverless) {
    HalFile sentinel;
    if (Storage.openFileForWrite("COVER", outPath, sentinel)) {
      const uint8_t marker = 'X';
      sentinel.write(&marker, 1);
    }
  } else {
    LOG_ERR("COVER", "Thumbnail generation failed (will retry): %s", epubPath.c_str());
  }
  return false;
}

bool readMetadata(const std::string& epubPath, std::string* titleOut, std::string* authorOut) {
  return withOpenBook(epubPath, [&](Book& book, freeink::book::BookSource&, Arena&) {
    if (titleOut != nullptr) *titleOut = book.metadata().title;
    if (authorOut != nullptr) *authorOut = book.metadata().author;
    return true;
  });
}

}  // namespace BookCoverUtils
