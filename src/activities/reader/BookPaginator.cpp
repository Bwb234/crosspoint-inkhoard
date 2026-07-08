#include "BookPaginator.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <text/hyph_en_us.h>

#include <cstring>

#include "CrossPointSettings.h"

using freeink::book::Arena;
using freeink::book::BookStatus;
using freeink::book::ChapterLayout;
using freeink::book::CssStylesheetBuilder;
using freeink::book::ManifestItem;
using freeink::book::PageCacheWriter;
using freeink::book::TextAlign;
using freeink::book::ZipCatalog;

namespace {

// The .cpfont ladder: FONT_SIZE setting index -> pixel size. Must stay in
// step with the NOTOSERIF_12..18 / NOTOSANS_12..18 font registrations.
constexpr uint16_t kLadderPx[CrossPointSettings::FONT_SIZE_COUNT] = {12, 14, 16, 18};

uint32_t hashMix(uint32_t hash, uint32_t value) {
  hash ^= value;
  return hash * 16777619u;
}

// Forwards pages to the cache writer while surfacing build progress to the
// UI (indexing popup) every 16 pages.
class ProgressSink : public freeink::book::PageSink {
 public:
  ProgressSink(PageCacheWriter& writer, const BookPaginator::BuildProgress& progress)
      : writer_(writer), progress_(progress) {}
  void onAnchor(const uint32_t idHash, const uint32_t charStart) override { writer_.onAnchor(idHash, charStart); }
  bool onPage(const freeink::book::Page& page) override {
    const bool ok = writer_.onPage(page);
    if (ok && progress_.fn != nullptr && (writer_.pageCount() & 15u) == 0) {
      progress_.fn(progress_.ctx, writer_.pageCount());
    }
    return ok;
  }

 private:
  PageCacheWriter& writer_;
  BookPaginator::BuildProgress progress_;
};

}  // namespace

bool BookPaginator::open(const std::string& path, const std::string& cacheDir, GfxRenderer& renderer,
                         const bool forcePlainText) {
  close();

  if (!source_.open(path.c_str())) {
    LOG_ERR("FIB", "Cannot open book file: %s", path.c_str());
    close();
    return false;
  }

  const size_t len = path.size();
  isTxt_ = forcePlainText || (len > 4 && strcasecmp(path.c_str() + len - 4, ".txt") == 0);

  if (!isTxt_) {
    // The whole book budget must coexist with a ~100+ KB chapter-build
    // scratch on a heap where the reader sees ~140 KB free, so worst-case
    // arena sizing is unaffordable. Open in two passes: parse into a
    // full-size TEMPORARY arena to learn the book's real footprint (corpus
    // high-water is 5-40 KB against the 48 KB cap), then re-open into an
    // exactly-sized buffer. The stylesheet is likewise compacted down to its
    // real rule array. All open-time transients are freed before the
    // per-chapter arenas are allocated.
    auto scratchBuf = makeUniqueNoThrow<uint8_t[]>(kOpenScratchSize);
    auto tempBookBuf = makeUniqueNoThrow<uint8_t[]>(kBookArenaSize);
    if (!scratchBuf || !tempBookBuf) {
      LOG_ERR("FIB", "OOM: open working set (%u B, free heap %u)",
              static_cast<unsigned>(kOpenScratchSize + kBookArenaSize), static_cast<unsigned>(ESP.getFreeHeap()));
      close();
      return false;
    }
    Arena scratch(scratchBuf.get(), kOpenScratchSize);
    bookArena_.init(tempBookBuf.get(), kBookArenaSize);

    BookStatus st = book_.open(source_, bookArena_, scratch);
    if (st != BookStatus::Ok) {
      LOG_ERR("FIB", "Book open failed: %d (%s)", static_cast<int>(st), path.c_str());
      close();
      return false;
    }

    // Second pass into the exact footprint (+ slack for alignment). The
    // temporary arena is freed FIRST — the reopen re-parses the container
    // rather than copying, so peak in-flight stays at scratch + one arena.
    const size_t bookUsed = bookArena_.used();
    tempBookBuf.reset();
    book_ = freeink::book::Book();
    bookBuf_ = makeUniqueNoThrow<uint8_t[]>(bookUsed + 128);
    if (!bookBuf_) {
      LOG_ERR("FIB", "OOM: book arena reopen (%u B)", static_cast<unsigned>(bookUsed + 128));
      close();
      return false;
    }
    bookArena_.init(bookBuf_.get(), bookUsed + 128);
    st = book_.open(source_, bookArena_, scratch);
    if (st != BookStatus::Ok) {
      LOG_ERR("FIB", "Exact-size reopen failed: %d", static_cast<int>(st));
      close();
      return false;
    }

    // Stylesheet: build in a temporary working arena, keep only the rules.
    auto tempSheetBuf = makeUniqueNoThrow<uint8_t[]>(kSheetArenaSize);
    if (tempSheetBuf) {
      Arena sheetArena(tempSheetBuf.get(), kSheetArenaSize);
      CssStylesheetBuilder builder;
      if (builder.begin(sheetArena)) {
        for (size_t m = 0; m < book_.manifestCount(); ++m) {
          const ManifestItem* item = book_.manifestItem(m);
          if (item == nullptr || item->mediaType == nullptr || strcmp(item->mediaType, "text/css") != 0) continue;
          if (const freeink::book::ZipEntry* e = book_.zip().find(item->href)) {
            builder.addSheet(source_, *e, scratch);
          }
        }
        sheet_ = builder.finish();
        if (builder.skippedSheets() > 0) {
          LOG_INF("FIB", "%u stylesheet(s) skipped (size cap)", builder.skippedSheets());
        }
        if (sheet_.ruleCount > 0) {
          // CssRule is self-contained (name hashes + POD declaration), so the
          // rule array can move out of the 12 KB working arena wholesale.
          const size_t rulesBytes = sheet_.ruleCount * sizeof(freeink::book::CssRule);
          sheetBuf_ = makeUniqueNoThrow<uint8_t[]>(rulesBytes);
          if (sheetBuf_) {
            memcpy(sheetBuf_.get(), sheet_.rules, rulesBytes);
            sheet_.rules = reinterpret_cast<const freeink::book::CssRule*>(sheetBuf_.get());
          } else {
            sheet_ = freeink::book::CssStylesheet{};  // OOM: lay out with element defaults
          }
        }
      }
    }

    LOG_INF("FIB", "Book open: %u spine items, book arena %u B (exact), %u CSS rules, free heap %u",
            static_cast<unsigned>(book_.spineCount()), static_cast<unsigned>(bookUsed), sheet_.ruleCount,
            static_cast<unsigned>(ESP.getFreeHeap()));
  }

  cache_.setDir(cacheDir.c_str());

  if (!buildFontChain(renderer)) {
    close();
    return false;
  }
  loadHyphenator();

  // Per-chapter arenas last, once the open-time transients are gone.
  if (!reallocChapterArenas()) {
    close();
    return false;
  }

  open_ = true;
  return true;
}

void BookPaginator::close() {
  source_.close();
  reader_ = freeink::book::PageCacheReader();
  book_ = freeink::book::Book();
  sheet_ = freeink::book::CssStylesheet{};
  chain_ = freeink::book::FontChain();
  hyphBlob_.reset();
  bookBuf_.reset();
  indexBuf_.reset();
  pageBuf_.reset();
  sheetBuf_.reset();
  ladderCount_ = 0;
  curSpine_ = kNoSpine;
  open_ = false;
  isTxt_ = false;
}

const char* BookPaginator::language() const {
  if (!isTxt_ && book_.metadata().language != nullptr && book_.metadata().language[0] != '\0') {
    return book_.metadata().language;
  }
  return "en";
}

bool BookPaginator::buildFontChain(GfxRenderer& renderer) {
  static constexpr freeink::book::StyleFlags kChainFlags[4] = {
      freeink::book::StyleNone,
      freeink::book::StyleBold,
      freeink::book::StyleItalic,
      static_cast<freeink::book::StyleFlags>(freeink::book::StyleBold | freeink::book::StyleItalic),
  };

  ladderCount_ = 0;
  const auto& fontMap = renderer.getFontMap();
  for (uint8_t s = 0; s < CrossPointSettings::FONT_SIZE_COUNT; ++s) {
    const int fontId = SETTINGS.getReaderFontId(s);
    const auto it = fontMap.find(fontId);
    if (it == fontMap.end()) {
      LOG_ERR("FIB", "Reader font id %d (size %u px) not registered", fontId, kLadderPx[s]);
      continue;
    }
    for (auto& adapter : adapters_) {
      adapter.addSize(kLadderPx[s], &it->second);
    }
    ladderFontIds_[ladderCount_] = fontId;
    ladderSizes_[ladderCount_] = kLadderPx[s];
    ++ladderCount_;
  }
  if (ladderCount_ == 0) {
    LOG_ERR("FIB", "No reader fonts available");
    return false;
  }
  for (uint8_t i = 0; i < 4; ++i) {
    chain_.add(&adapters_[i], kChainFlags[i]);
  }
  return true;
}

void BookPaginator::loadHyphenator() {
  if (!SETTINGS.hyphenationEnabled) return;

  // Prefer language-specific patterns from the SD card (compiled with the
  // SDK's tools/hyphc.py); English ships embedded in flash. A missing blob
  // degrades to the embedded en-US patterns, which simply never match
  // non-Latin words — text still lays out, just unhyphenated.
  const char* lang = language();
  char two[3] = {0, 0, 0};
  two[0] = lang[0];
  two[1] = lang[1] != '\0' ? lang[1] : '\0';
  if (strncasecmp(two, "en", 2) != 0) {
    char blobPath[64];
    snprintf(blobPath, sizeof(blobPath), "/hyphenation/hyph-%c%c.fibh", two[0], two[1]);
    HalFile f;
    if (Storage.openFileForRead("FIB", blobPath, f)) {
      const size_t size = f.fileSize();
      hyphBlob_ = makeUniqueNoThrow<uint8_t[]>(size);
      if (hyphBlob_ && f.read(hyphBlob_.get(), size) == static_cast<int>(size) &&
          hyphenator_.init(hyphBlob_.get(), static_cast<uint32_t>(size))) {
        LOG_INF("FIB", "Hyphenation patterns: %s (%u B)", blobPath, static_cast<unsigned>(size));
        return;
      }
      hyphBlob_.reset();
      LOG_ERR("FIB", "Failed to load %s; falling back to embedded en-US", blobPath);
    }
  }
  hyphenator_.init(freeink::book::k_hyph_en_us, sizeof(freeink::book::k_hyph_en_us));
}

void BookPaginator::configureLayout(const int16_t pageWidth, const int16_t pageHeight, const int16_t marginLeft,
                                    const int16_t marginRight, const int16_t marginTop, const int16_t marginBottom) {
  params_ = freeink::book::LayoutParams{};
  params_.pageWidth = pageWidth;
  params_.pageHeight = pageHeight;
  params_.marginLeft = marginLeft;
  params_.marginRight = marginRight;
  params_.marginTop = marginTop;
  params_.marginBottom = marginBottom;

  const uint8_t sizeIndex =
      SETTINGS.fontSize < CrossPointSettings::FONT_SIZE_COUNT ? SETTINGS.fontSize : CrossPointSettings::MEDIUM;
  params_.baseSizePx = kLadderPx[sizeIndex];
  params_.font = &chain_;
  params_.stylesheet = (!isTxt_ && sheet_.ruleCount > 0) ? &sheet_ : nullptr;
  params_.language = language();

  params_.lineSpacingPct = static_cast<uint16_t>(SETTINGS.getReaderLineCompression() * 100.0f + 0.5f);
  params_.paragraphSpacingPct = SETTINGS.extraParagraphSpacing ? 150 : 100;
  params_.embeddedStyles = SETTINGS.embeddedStyle != 0;
  params_.focusReading = SETTINGS.focusReadingEnabled != 0;
  params_.hyphenator = (SETTINGS.hyphenationEnabled && hyphenator_.ready()) ? &hyphenator_ : nullptr;

  switch (SETTINGS.paragraphAlignment) {
    case CrossPointSettings::LEFT_ALIGN:
      params_.defaultAlign = TextAlign::Left;
      break;
    case CrossPointSettings::CENTER_ALIGN:
      params_.defaultAlign = TextAlign::Center;
      break;
    case CrossPointSettings::RIGHT_ALIGN:
      params_.defaultAlign = TextAlign::Right;
      break;
    case CrossPointSettings::BOOK_STYLE:
      // Publisher's choice: left when the book CSS is silent.
      params_.defaultAlign = TextAlign::Left;
      break;
    case CrossPointSettings::JUSTIFIED:
    default:
      params_.defaultAlign = TextAlign::Justify;
      break;
  }
}

// (Re)allocates the per-chapter arenas released around a build's big scratch
// block. On failure the paginator stays book-open but chapter-less: callers
// see chapterReady() == false and surface the error.
bool BookPaginator::reallocChapterArenas() {
  if (!indexBuf_) indexBuf_ = makeUniqueNoThrow<uint8_t[]>(kIndexArenaSize);
  if (!pageBuf_) pageBuf_ = makeUniqueNoThrow<uint8_t[]>(kPageArenaSize);
  if (!indexBuf_ || !pageBuf_) {
    LOG_ERR("FIB", "OOM: chapter arenas (%u B)", static_cast<unsigned>(kIndexArenaSize + kPageArenaSize));
    indexBuf_.reset();
    pageBuf_.reset();
    return false;
  }
  indexArena_.init(indexBuf_.get(), kIndexArenaSize);
  pageArena_.init(pageBuf_.get(), kPageArenaSize);
  return true;
}

uint32_t BookPaginator::fontFingerprint() const {
  uint32_t hash = 2166136261u;
  for (uint8_t i = 0; i < ladderCount_; ++i) {
    hash = hashMix(hash, static_cast<uint32_t>(ladderFontIds_[i]));
    hash = hashMix(hash, ladderSizes_[i]);
  }
  return hash;
}

uint32_t BookPaginator::generation() const { return freeink::book::layoutGenerationHash(params_, fontFingerprint()); }

freeink::book::BookStatus BookPaginator::ensureChapter(const uint16_t spineIndex, const BuildProgress& progress) {
  const uint32_t gen = generation();
  if (!freeink::book::pageCacheName(spineIndex, gen, cacheName_, sizeof(cacheName_))) {
    return BookStatus::Unsupported;
  }

  indexArena_.reset();
  curSpine_ = kNoSpine;
  BookStatus st = reader_.open(cache_, cacheName_, gen, indexArena_);
  if (st == BookStatus::Ok) {
    curSpine_ = spineIndex;
    return st;
  }

  const ManifestItem* item = isTxt_ ? nullptr : book_.spineItem(spineIndex);
  const freeink::book::ZipEntry* entry = (!isTxt_ && item != nullptr) ? book_.zip().find(item->href) : nullptr;
  if (!isTxt_ && entry == nullptr) return BookStatus::NotFound;
  if (isTxt_ && spineIndex != 0) return BookStatus::NotFound;

  // The pagination working set (layout buffers + one inflate stream + the
  // writer's page index) lives only for this call — but it needs one big
  // CONTIGUOUS block, and everything else the parse touches does NOT come
  // from this arena: expat's internal pools allocate from the system heap.
  // Grabbing the largest possible block starved exactly that — the heap
  // dipped to ~6 KB mid-parse and a failed expat malloc surfaces as a bogus
  // ParseError. So size adaptively: as much as available AFTER a reserve for
  // the parser's heap use, capped at the ideal, floored at what an ordinary
  // text chapter needs. Free the idle per-chapter arenas first so their
  // space can coalesce into the block.
  reader_ = freeink::book::PageCacheReader();
  indexBuf_.reset();
  pageBuf_.reset();

  // Expat pools + misc system-heap use during the parse. Measured on device:
  // a 28 KB reserve bottomed out at ~8-10 KB free mid-build, so ~19 KB is
  // real parser use; 36 KB keeps a comfortable floor without starving the
  // scratch (typical chapters use ~76 KB of it).
  constexpr size_t kParserHeapReserve = 36 * 1024;
  constexpr size_t kScratchFloor = 88 * 1024;       // text-chapter working set + writer index

  const size_t freeHeap = ESP.getFreeHeap();
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t scratchSize = kBuildScratchSize;
  if (freeHeap > kParserHeapReserve && freeHeap - kParserHeapReserve < scratchSize) {
    scratchSize = freeHeap - kParserHeapReserve;
  }
  if (largestBlock > 64 && largestBlock - 64 < scratchSize) {  // allocator header slack
    scratchSize = largestBlock - 64;
  }
  std::unique_ptr<uint8_t[]> scratchBuf;
  if (scratchSize >= kScratchFloor) {
    scratchBuf = makeUniqueNoThrow<uint8_t[]>(scratchSize);
  }
  if (!scratchBuf) {
    LOG_ERR("FIB", "OOM: build scratch (want >= %u B, free heap %u, max block %u)",
            static_cast<unsigned>(kScratchFloor), static_cast<unsigned>(freeHeap), static_cast<unsigned>(largestBlock));
    reallocChapterArenas();  // restore the per-chapter arenas for the caller
    return BookStatus::OutOfMemory;
  }
  if (scratchSize != kBuildScratchSize) {
    LOG_INF("FIB", "Build scratch sized to %u B (free heap %u, max block %u)", static_cast<unsigned>(scratchSize),
            static_cast<unsigned>(freeHeap), static_cast<unsigned>(largestBlock));
  }
  Arena scratch(scratchBuf.get(), scratchSize);

  const uint32_t t0 = millis();
  {
    PageCacheWriter writer;
    if (!writer.begin(cache_, cacheName_, gen, scratch)) {
      scratchBuf.reset();
      reallocChapterArenas();
      return BookStatus::IoError;
    }

    ProgressSink sink(writer, progress);
    uint32_t totalChars = 0;
    st = isTxt_ ? ChapterLayout::layoutPlainText(source_, params_, scratch, sink, nullptr, &totalChars)
                : ChapterLayout::layout(source_, book_.zip(), *entry, item->href, params_, scratch, sink, nullptr,
                                        &totalChars);
    writer.setTotalChars(totalChars);
    if (st == BookStatus::Ok && !writer.finish()) st = BookStatus::IoError;
    if (st != BookStatus::Ok) {
      LOG_ERR("FIB", "Chapter %u layout failed: %d (scratch high water %u/%u B)", spineIndex, static_cast<int>(st),
              static_cast<unsigned>(scratch.highWater()), static_cast<unsigned>(scratchSize));
      scratchBuf.reset();
      reallocChapterArenas();
      return st;
    }
    LOG_INF("FIB", "Chapter %u paginated: %u pages in %ums (scratch high water %u/%u B, free heap %u)", spineIndex,
            writer.pageCount(), static_cast<unsigned>(millis() - t0), static_cast<unsigned>(scratch.highWater()),
            static_cast<unsigned>(scratchSize), static_cast<unsigned>(ESP.getFreeHeap()));
  }
  scratchBuf.reset();
  if (!reallocChapterArenas()) return BookStatus::OutOfMemory;

  indexArena_.reset();
  st = reader_.open(cache_, cacheName_, gen, indexArena_);
  if (st == BookStatus::Ok) curSpine_ = spineIndex;
  return st;
}

bool BookPaginator::charForAnchor(const char* fragment, uint32_t* charOut) const {
  if (fragment == nullptr || fragment[0] == '\0') return false;
  return reader_.charForAnchor(ZipCatalog::hashPath(fragment), charOut);
}

freeink::book::BookStatus BookPaginator::readPage(const uint32_t pageIndex, freeink::book::Page* out) {
  // pageBuf_ is released transiently around chapter builds; a null here means
  // a failed reallocation (the arena would point at freed memory).
  if (curSpine_ == kNoSpine || !pageBuf_) return BookStatus::NotFound;
  pageArena_.reset();
  return reader_.readPage(pageIndex, pageArena_, out);
}

int BookPaginator::spineIndexForHref(const char* href) const {
  if (href == nullptr || href[0] == '\0' || isTxt_) return -1;
  for (size_t s = 0; s < book_.spineCount(); ++s) {
    const ManifestItem* item = book_.spineItem(s);
    if (item != nullptr && strcmp(item->href, href) == 0) return static_cast<int>(s);
  }
  return -1;
}

BookPaginator::TocItem BookPaginator::tocItem(const size_t index) const {
  TocItem out{"", nullptr, -1, 0};
  const freeink::book::TocEntry* entry = isTxt_ ? nullptr : book_.tocEntry(index);
  if (entry == nullptr) return out;
  out.title = entry->title;
  out.fragment = entry->fragment;
  out.depth = entry->depth;
  out.spineIndex = spineIndexForHref(entry->href);
  return out;
}

int BookPaginator::tocIndexForSpine(const int spineIndex) const {
  // The chapter's title is the last TOC entry at or before this spine item
  // (a spine item without its own entry belongs to the preceding heading).
  int best = -1;
  int bestSpine = -1;
  for (size_t t = 0; t < tocCount(); ++t) {
    const int s = tocItem(t).spineIndex;
    if (s < 0 || s > spineIndex) continue;
    if (s >= bestSpine) {
      bestSpine = s;
      best = static_cast<int>(t);
    }
  }
  return best;
}

// Spine weights use the uncompressed sizes already in the ZIP catalog — the
// same "bigger chapters cover more of the book" heuristic the legacy engine
// used, with zero extra state.
float BookPaginator::bookProgress(const int spineIndex, const float chapterFraction) const {
  if (isTxt_) return chapterFraction;
  uint64_t before = 0;
  uint64_t current = 0;
  uint64_t total = 0;
  for (size_t s = 0; s < book_.spineCount(); ++s) {
    const ManifestItem* item = book_.spineItem(s);
    const freeink::book::ZipEntry* e = item != nullptr ? book_.zip().find(item->href) : nullptr;
    const uint32_t size = e != nullptr ? e->uncompressedSize : 0;
    if (static_cast<int>(s) < spineIndex) before += size;
    if (static_cast<int>(s) == spineIndex) current = size;
    total += size;
  }
  if (total == 0) return 0.0f;
  const float f = chapterFraction < 0.0f ? 0.0f : (chapterFraction > 1.0f ? 1.0f : chapterFraction);
  return (static_cast<float>(before) + f * static_cast<float>(current)) / static_cast<float>(total);
}

int BookPaginator::spineForBookFraction(const float bookFraction, float* chapterFractionOut) const {
  if (chapterFractionOut != nullptr) *chapterFractionOut = 0.0f;
  if (isTxt_ || book_.spineCount() == 0) {
    if (chapterFractionOut != nullptr) *chapterFractionOut = bookFraction;
    return 0;
  }
  uint64_t total = 0;
  for (size_t s = 0; s < book_.spineCount(); ++s) {
    const ManifestItem* item = book_.spineItem(s);
    const freeink::book::ZipEntry* e = item != nullptr ? book_.zip().find(item->href) : nullptr;
    total += e != nullptr ? e->uncompressedSize : 0;
  }
  const float f = bookFraction < 0.0f ? 0.0f : (bookFraction > 1.0f ? 1.0f : bookFraction);
  const uint64_t target = static_cast<uint64_t>(f * static_cast<float>(total));
  uint64_t cumulative = 0;
  for (size_t s = 0; s < book_.spineCount(); ++s) {
    const ManifestItem* item = book_.spineItem(s);
    const freeink::book::ZipEntry* e = item != nullptr ? book_.zip().find(item->href) : nullptr;
    const uint32_t size = e != nullptr ? e->uncompressedSize : 0;
    if (target < cumulative + size || s + 1 == book_.spineCount()) {
      if (chapterFractionOut != nullptr && size > 0) {
        *chapterFractionOut = static_cast<float>(target - cumulative) / static_cast<float>(size);
      }
      return static_cast<int>(s);
    }
    cumulative += size;
  }
  return 0;
}

int BookPaginator::fontIdForRunSize(const uint16_t sizePx) const {
  const uint16_t q = adapters_[0].quantize(sizePx);
  for (uint8_t i = 0; i < ladderCount_; ++i) {
    if (ladderSizes_[i] == q) return ladderFontIds_[i];
  }
  return ladderCount_ > 0 ? ladderFontIds_[0] : 0;
}
