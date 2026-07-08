#include "BookPaginator.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <text/hyph_en_us.h>

#include <algorithm>
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
  cacheDir_ = cacheDir;
  close();

  if (!source_.open(path.c_str())) {
    LOG_ERR("FIB", "Cannot open book file: %s", path.c_str());
    close();
    return false;
  }

  const size_t len = path.size();
  isTxt_ = forcePlainText || (len > 4 && strcasecmp(path.c_str() + len - 4, ".txt") == 0);

  // The cache dir hosts both the page caches and (for omnibuses) the
  // container index, so bind it before the catalog fast path below.
  cache_.setDir(cacheDir.c_str());

  if (!isTxt_) {
    bool fbLent = false;

    // Catalog fast path: an existing catalog.fibc means this container
    // already outgrew the in-RAM probe ladder on a previous open -- load the
    // compact resident tables and skip the ladder entirely. A stale index
    // (container changed) is removed inside openCatalog() and the ladder
    // below decides afresh what the book now needs.
    if (cache_.exists(freeink::book::BookCatalog::kCatalogName)) {
      catalogMode_ = openCatalog();
    }
    // The whole book budget must coexist with a ~100+ KB chapter-build
    // scratch on a heap where the reader sees ~140 KB free, so worst-case
    // arena sizing is unaffordable. Open in two passes: parse into a
    // full-size TEMPORARY arena to learn the book's real footprint (corpus
    // high-water is 5-40 KB against the 48 KB cap), then re-open into an
    // exactly-sized buffer. The stylesheet is likewise compacted down to its
    // real rule array. All open-time transients are freed before the
    // per-chapter arenas are allocated.
    // PHASE A: parse into full-size temporaries to learn the real footprint,
    // then free EVERYTHING. Persistents allocated into the emptied heap take
    // the start of the big free region (or a pocket), leaving the tail
    // contiguous — allocating them while transients are held, or keeping the
    // temp arena, both proved to pin large blocks mid-heap and split the
    // region the chapter builds need.
    size_t bookUsed = 0;
    if (!catalogMode_) {
      // Probe ladder: most books fit the 48 KB rung; webnovel omnibuses
      // (1500+ spine items) need far more container metadata, so the bigger
      // rungs borrow the framebuffer (the LOADING popup already on the panel
      // survives — e-ink holds its image without the buffer).
      static constexpr size_t kProbeSizes[] = {kBookArenaSize, 72 * 1024, 96 * 1024};
      BookStatus st = BookStatus::OutOfMemory;
      for (size_t attempt = 0; attempt < sizeof(kProbeSizes) / sizeof(kProbeSizes[0]); ++attempt) {
        const size_t probeSize = kProbeSizes[attempt];
        if (attempt > 0 && !fbLent) {
          renderer.releaseFrameBufferForBuild();
          fbLent = true;
        }
        auto scratchBuf = makeUniqueNoThrow<uint8_t[]>(kOpenScratchSize);
        auto tempBookBuf = makeUniqueNoThrow<uint8_t[]>(probeSize);
        if (!scratchBuf || !tempBookBuf) {
          LOG_ERR("FIB", "OOM: open working set (%u B, free heap %u)",
                  static_cast<unsigned>(kOpenScratchSize + probeSize), static_cast<unsigned>(ESP.getFreeHeap()));
          st = BookStatus::OutOfMemory;
          break;  // the heap itself is short; a bigger rung cannot help
        }
        Arena scratch(scratchBuf.get(), kOpenScratchSize);
        Arena tempArena(tempBookBuf.get(), probeSize);
        freeink::book::Book probeBook;
        st = probeBook.open(source_, tempArena, scratch);
        if (st == BookStatus::Ok) {
          bookUsed = tempArena.used();
          if (attempt > 0) {
            LOG_INF("FIB", "Large container: book arena needs %u B (attempt %u)", static_cast<unsigned>(bookUsed),
                    static_cast<unsigned>(attempt + 1));
          }
          break;
        }
        if (st != BookStatus::OutOfMemory) break;  // real parse error, not arena size
        LOG_DBG("FIB", "Book probe outgrew %u B, retrying larger", static_cast<unsigned>(probeSize));
      }
      if (st == BookStatus::OutOfMemory) {
        // Even the 96 KB rung overflowed: webnovel-omnibus territory (1,700+
        // spine items need ~400 KB of container metadata). Build the
        // SD-backed catalog instead -- compact resident tables, everything
        // string-shaped stays on the card. The framebuffer is already lent
        // unless the heap itself was short, in which case the catalog build
        // fails with its own clear log.
        if (!fbLent) {
          renderer.releaseFrameBufferForBuild();
          fbLent = true;
        }
        LOG_INF("FIB", "Container outgrew the probe ladder; building SD catalog");
        catalogMode_ = buildCatalog() && openCatalog();
        if (catalogMode_) st = BookStatus::Ok;
      }
      if (st != BookStatus::Ok) {
        LOG_ERR("FIB", "Book open failed: %d (%s)", static_cast<int>(st), path.c_str());
        if (fbLent && !renderer.restoreFrameBufferAfterBuild()) ESP.restart();
        close();
        return false;
      }
    }

    if (!catalogMode_) {
      // PHASE B: exact-size arena into the emptied heap, re-parse for keeps.
      bookBuf_ = makeUniqueNoThrow<uint8_t[]>(bookUsed + 128);
      auto scratchBuf = makeUniqueNoThrow<uint8_t[]>(kOpenScratchSize);
      if (!bookBuf_ || !scratchBuf) {
        LOG_ERR("FIB", "OOM: book arena reopen (%u B)", static_cast<unsigned>(bookUsed + 128));
        if (fbLent && !renderer.restoreFrameBufferAfterBuild()) ESP.restart();
        close();
        return false;
      }
      Arena scratch(scratchBuf.get(), kOpenScratchSize);
      bookArena_.init(bookBuf_.get(), bookUsed + 128);
      const BookStatus st = book_.open(source_, bookArena_, scratch);
      if (st != BookStatus::Ok) {
        LOG_ERR("FIB", "Exact-size reopen failed: %d", static_cast<int>(st));
        if (fbLent && !renderer.restoreFrameBufferAfterBuild()) ESP.restart();
        close();
        return false;
      }
      buildStylesheet(scratch);
      scratchBuf.reset();  // free the reopen scratch before the framebuffer returns
    } else if (catalog_.cssCount() > 0) {
      // Catalog mode: CSS entries were resolved at catalog open; the build
      // only needs a transient inflate-capable scratch.
      auto scratchBuf = makeUniqueNoThrow<uint8_t[]>(kOpenScratchSize);
      if (scratchBuf) {
        Arena scratch(scratchBuf.get(), kOpenScratchSize);
        buildStylesheet(scratch);
      } else {
        LOG_ERR("FIB", "OOM: stylesheet scratch; using element defaults");
      }
    }

    if (fbLent && !renderer.restoreFrameBufferAfterBuild()) {
      LOG_ERR("FIB", "Framebuffer restore failed after large open");
      ESP.restart();  // transients freed; a failed 48 KB realloc means heap corruption
    }

    LOG_INF("FIB", "Book open%s: %u spine items, book arena %u B, %u CSS rules, free heap %u",
            catalogMode_ ? " (SD catalog)" : "", static_cast<unsigned>(spineCount()),
            static_cast<unsigned>(bookArena_.used()), sheet_.ruleCount, static_cast<unsigned>(ESP.getFreeHeap()));
  }

  if (!buildFontChain(renderer)) {
    close();
    return false;
  }
  loadHyphenator();

  // Per-chapter arenas are allocated lazily by ensureChapter(): a first-open
  // chapter build would only free them again, and every idle KB at build
  // time is scratch budget.
  open_ = true;
  return true;
}

// Loads an existing catalog.fibc into an exactly-sized resident arena. A
// stale index (container changed since the build) is deleted so the caller
// can rebuild; any other failure just reports false.
bool BookPaginator::openCatalog() {
  size_t resident = 0;
  BookStatus st = freeink::book::BookCatalog::residentBytes(cache_, &resident);
  if (st != BookStatus::Ok) return false;

  bookBuf_ = makeUniqueNoThrow<uint8_t[]>(resident);
  auto scratchBuf = makeUniqueNoThrow<uint8_t[]>(4096);  // fingerprint scan buffer
  if (!bookBuf_ || !scratchBuf) {
    LOG_ERR("FIB", "OOM: catalog resident arena (%u B)", static_cast<unsigned>(resident));
    bookBuf_.reset();
    return false;
  }
  bookArena_.init(bookBuf_.get(), resident);
  Arena scratch(scratchBuf.get(), 4096);
  st = catalog_.open(source_, cache_, bookArena_, scratch);
  if (st == BookStatus::Stale) {
    LOG_INF("FIB", "Catalog stale (container changed); rebuilding");
    cache_.remove(freeink::book::BookCatalog::kCatalogName);
    bookBuf_.reset();
    return false;
  }
  if (st != BookStatus::Ok) {
    LOG_ERR("FIB", "Catalog open failed: %d", static_cast<int>(st));
    bookBuf_.reset();
    return false;
  }
  LOG_INF("FIB", "SD catalog open: %u spines, %u toc, resident %u B", static_cast<unsigned>(catalog_.spineCount()),
          static_cast<unsigned>(catalog_.tocCount()), static_cast<unsigned>(bookArena_.used()));
  return true;
}

// Streams the container index to SD. Caller has the framebuffer lent; the
// two arenas mirror the chapter-build split (record tables vs parse stream).
// Measured for a 1,732-spine omnibus: records ~72.5 KB, parse ~45.3 KB.
bool BookPaginator::buildCatalog() {
  constexpr size_t kRecordsIdeal = 96 * 1024;
  constexpr size_t kRecordsFloor = 76 * 1024;
  constexpr size_t kParseSize = 50 * 1024;
  constexpr size_t kAllocSlack = 64;

  auto parseBuf = makeUniqueNoThrow<uint8_t[]>(kParseSize);
  if (!parseBuf) {
    LOG_ERR("FIB", "OOM: catalog parse arena (free heap %u)", static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }
  const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t recordsSize = std::min(kRecordsIdeal, block > kAllocSlack ? block - kAllocSlack : 0);
  std::unique_ptr<uint8_t[]> recordsBuf;
  if (recordsSize >= kRecordsFloor) recordsBuf = makeUniqueNoThrow<uint8_t[]>(recordsSize);
  if (!recordsBuf) {
    LOG_ERR("FIB", "OOM: catalog record arena (want %u B, max block %u)", static_cast<unsigned>(recordsSize),
            static_cast<unsigned>(block));
    return false;
  }
  Arena records(recordsBuf.get(), recordsSize);
  Arena parse(parseBuf.get(), kParseSize);

  const uint32_t t0 = millis();
  const BookStatus st = freeink::book::BookCatalog::build(source_, cache_, records, &parse);
  if (st != BookStatus::Ok) {
    LOG_ERR("FIB", "Catalog build failed: %d (records %u/%u refused %u, parse %u/%u refused %u B)",
            static_cast<int>(st), static_cast<unsigned>(records.highWater()), static_cast<unsigned>(recordsSize),
            static_cast<unsigned>(records.failedAllocSize()), static_cast<unsigned>(parse.highWater()),
            static_cast<unsigned>(kParseSize), static_cast<unsigned>(parse.failedAllocSize()));
    return false;
  }
  LOG_INF("FIB", "Catalog built in %ums (records %u/%u, parse %u/%u B)", static_cast<unsigned>(millis() - t0),
          static_cast<unsigned>(records.highWater()), static_cast<unsigned>(recordsSize),
          static_cast<unsigned>(parse.highWater()), static_cast<unsigned>(kParseSize));
  return true;
}

// Builds the book stylesheet in a temporary working arena and keeps only the
// compacted rule array. CSS items come from whichever catalog is active.
void BookPaginator::buildStylesheet(Arena& scratch) {
  auto tempSheetBuf = makeUniqueNoThrow<uint8_t[]>(kSheetArenaSize);
  if (!tempSheetBuf) return;
  Arena sheetArena(tempSheetBuf.get(), kSheetArenaSize);
  CssStylesheetBuilder builder;
  if (!builder.begin(sheetArena)) return;
  if (catalogMode_) {
    for (size_t c = 0; c < catalog_.cssCount(); ++c) {
      builder.addSheet(source_, *catalog_.cssEntry(c), scratch);
    }
  } else {
    for (size_t m = 0; m < book_.manifestCount(); ++m) {
      const ManifestItem* item = book_.manifestItem(m);
      if (item == nullptr || item->mediaType == nullptr || strcmp(item->mediaType, "text/css") != 0) continue;
      if (const freeink::book::ZipEntry* e = book_.zip().find(item->href)) {
        builder.addSheet(source_, *e, scratch);
      }
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

void BookPaginator::close() {
  suspendBuild();  // commits a partial so the next open resumes instantly
  chapterSource_.close();
  source_.close();
  reader_ = freeink::book::PageCacheReader();
  book_ = freeink::book::Book();
  catalog_ = freeink::book::BookCatalog();
  catalogMode_ = false;
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
  const char* lang = catalogMode_ ? catalog_.metadata().language : book_.metadata().language;
  if (!isTxt_ && lang != nullptr && lang[0] != '\0') return lang;
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

bool BookPaginator::isChapterCached(const uint16_t spineIndex) {
  char name[sizeof(cacheName_)];
  if (!freeink::book::pageCacheName(spineIndex, generation(), name, sizeof(name))) return false;
  return cache_.exists(name);
}

freeink::book::BookStatus BookPaginator::ensureChapter(const uint16_t spineIndex, const BuildProgress& progress,
                                                       const uint32_t targetChar, const float targetFraction) {
  const uint32_t gen = generation();
  if (!freeink::book::pageCacheName(spineIndex, gen, cacheName_, sizeof(cacheName_))) {
    return BookStatus::Unsupported;
  }

  // An in-progress incremental build of this same chapter/generation stays
  // live across ensureChapter calls (page turns re-enter here).
  if (building_) {
    if (buildGeneration_ == gen && curSpine_ == spineIndex) return BookStatus::Ok;
    suspendBuild();  // settings/spine changed under the build
  }

  curSpine_ = kNoSpine;
  partialReaderOpen_ = false;
  if (!indexBuf_ && !reallocChapterArenas()) return BookStatus::OutOfMemory;
  indexArena_.reset();
  BookStatus st = reader_.open(cache_, cacheName_, gen, indexArena_);
  if (st == BookStatus::Ok && !reader_.isPartial()) {
    curSpine_ = spineIndex;
    return st;
  }

  // Resolve the chapter's entry + href into MEMBERS: in catalog mode the
  // ZipEntry is materialized from the SD index (no arena-resident copy
  // exists), and the incremental session retains pointers across steps.
  const freeink::book::ZipEntry* entry = nullptr;
  if (!isTxt_) {
    if (catalogMode_) {
      if (catalog_.spineEntry(spineIndex, &curEntry_) != BookStatus::Ok ||
          catalog_.spineHref(spineIndex, chapterHref_, sizeof(chapterHref_)) != BookStatus::Ok) {
        return BookStatus::NotFound;
      }
    } else {
      const ManifestItem* item = book_.spineItem(spineIndex);
      const freeink::book::ZipEntry* e = item != nullptr ? book_.zip().find(item->href) : nullptr;
      if (e == nullptr) return BookStatus::NotFound;
      curEntry_ = *e;
      snprintf(chapterHref_, sizeof(chapterHref_), "%s", item->href);
    }
    entry = &curEntry_;
  }
  if (isTxt_ && spineIndex != 0) return BookStatus::NotFound;

  // A suspended partial serves its pages instantly while a fresh background
  // rebuild re-lays the chapter from the start (deterministic layout makes
  // the rebuilt prefix identical). The partial file stays readable during
  // the rebuild because writes stream to a temp name until commit.
  const bool incrementalLanding = targetChar != kBuildAll || targetFraction >= 0.0f;
  if (st == BookStatus::Ok && reader_.isPartial() && !isTxt_ && incrementalLanding) {
    partialReaderOpen_ = true;
    const BookStatus inc = startIncremental(spineIndex, *entry, targetChar, targetFraction, progress);
    if (inc == BookStatus::Ok) {
      curSpine_ = spineIndex;
      return BookStatus::Ok;
    }
    // Incremental could not start (extraction/OOM): fall through to the
    // blocking build below, which replaces the partial outright.
    partialReaderOpen_ = false;
  }

  // Giant uncached chapter with a known landing position: build only a
  // little past it and finish behind the reader.
  constexpr uint32_t kIncrementalThreshold = 96 * 1024;  // uncompressed bytes
  if (!isTxt_ && incrementalLanding && entry->uncompressedSize > kIncrementalThreshold) {
    reader_ = freeink::book::PageCacheReader();
    indexBuf_.reset();  // nothing to serve from it; reclaim 12 KB for the session
    const BookStatus inc = startIncremental(spineIndex, *entry, targetChar, targetFraction, progress);
    if (inc == BookStatus::Ok) {
      curSpine_ = spineIndex;
      return BookStatus::Ok;
    }
    // fall through to the blocking build
  }

  // The pagination working set lives only for this call, as TWO arenas:
  // layout buffers + the writer's index in one, the parse stream (inflate
  // window + decompressor + XML chunks) in the other. Splitting kills the
  // single-~100 KB-contiguous-block requirement that fragmentation kept
  // defeating — two ~50 KB blocks fit every heap shape observed so far
  // (including a 59 KB max block). Expat's pools still come from the system
  // heap, so a reserve stays out of both. Free the idle per-chapter arenas
  // first so their space is available.
  reader_ = freeink::book::PageCacheReader();
  indexBuf_.reset();
  pageBuf_.reset();

  // Measured (small profile, host + device): layout side high water ~45.3 KB
  // INCLUDING the writer's chunked index (~2.5 KB for a normal chapter);
  // parse side ~46 KB for a DEFLATED entry (inflate window + decompressor
  // are irreducible), ~4 KB for a STORED one. Expat system-heap use ~19 KB.
  // If ParseError appears on a book that fibchecks clean on host, the
  // reserve is being squeezed — raise it first.
  constexpr size_t kParserHeapReserve = 22 * 1024;
  constexpr size_t kLayoutIdeal = 60 * 1024;
  // Floor is measured-demand-plus-margin, not comfort. The LayoutEngine
  // itself now lives in this arena (session refactor moved it off the
  // stack): small-profile host high water incl. writer index is 48,656 B.
  constexpr size_t kLayoutFloor = 48 * 1024;
  constexpr size_t kParseIdeal = 50 * 1024;
  constexpr size_t kParseFloor = 46 * 1024;
  constexpr size_t kParseStored = 8 * 1024;  // stored entries skip inflate state
  constexpr size_t kAllocSlack = 64;         // TLSF block header/split overhead

  const bool stored = !isTxt_ && entry->method == 0;
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t budget = freeHeap > kParserHeapReserve ? freeHeap - kParserHeapReserve : 0;

  // The parse arena goes first: its size is rigid (inflate state), and
  // placing it lets TLSF spend a mid-sized pocket on it when one exists. The
  // layout arena then takes what the heap can actually give: both draws
  // usually carve out of the same largest block, so sizing from the block
  // that REMAINS after the parse arena landed is what makes the pair fit —
  // free-heap math alone kept promising space TLSF couldn't deliver. When
  // the leftover misses the layout floor and the parse arena is still above
  // ITS floor, shrink parse and re-measure (there is ~2 KB of allocator
  // overhead between the two draws no upfront formula gets right).
  const bool parseFlexible = !stored && !isTxt_;
  size_t parseSize = parseFlexible ? kParseIdeal : kParseStored;
  if (parseFlexible && parseSize + kLayoutFloor > budget) parseSize = kParseFloor;
  std::unique_ptr<uint8_t[]> parseBuf;
  std::unique_ptr<uint8_t[]> layoutBuf;
  size_t layoutSize = 0;
  size_t blockAfterParse = 0;
  for (;;) {
    parseBuf = makeUniqueNoThrow<uint8_t[]>(parseSize);
    if (!parseBuf && parseFlexible && parseSize > kParseFloor) {
      parseSize = kParseFloor;
      parseBuf = makeUniqueNoThrow<uint8_t[]>(parseSize);
    }
    if (!parseBuf) break;

    blockAfterParse = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const size_t freeNow = ESP.getFreeHeap();
    layoutSize = std::min(kLayoutIdeal, blockAfterParse > kAllocSlack ? blockAfterParse - kAllocSlack : 0);
    layoutSize = std::min(layoutSize, freeNow > kParserHeapReserve ? freeNow - kParserHeapReserve : 0);
    if (layoutSize < kLayoutFloor && parseFlexible && parseSize > kParseFloor) {
      parseBuf.reset();  // give the block back, retry with the smaller parse arena
      parseSize = kParseFloor;
      continue;
    }
    if (layoutSize >= kLayoutFloor) layoutBuf = makeUniqueNoThrow<uint8_t[]>(layoutSize);
    break;
  }
  if (!layoutBuf || !parseBuf) {
    LOG_ERR("FIB", "OOM: build arenas (want %u+%u B, free heap %u, max block %u, after parse %u)",
            static_cast<unsigned>(layoutSize), static_cast<unsigned>(parseSize), static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(largestBlock), static_cast<unsigned>(blockAfterParse));
    layoutBuf.reset();
    parseBuf.reset();
    reallocChapterArenas();  // restore the per-chapter arenas for the caller
    return BookStatus::OutOfMemory;
  }
  Arena scratch(layoutBuf.get(), layoutSize);
  Arena parseArena(parseBuf.get(), parseSize);

  const uint32_t t0 = millis();
  {
    PageCacheWriter writer;
    if (!writer.begin(cache_, cacheName_, gen, scratch)) {
      layoutBuf.reset();
      parseBuf.reset();
      reallocChapterArenas();
      return BookStatus::IoError;
    }

    ProgressSink sink(writer, progress);
    uint32_t totalChars = 0;
    st = isTxt_ ? ChapterLayout::layoutPlainText(source_, params_, scratch, sink, nullptr, &totalChars)
                : ChapterLayout::layout(source_, zip(), *entry, chapterHref_, params_, scratch, sink, nullptr,
                                        &totalChars, &parseArena);
    writer.setTotalChars(totalChars);
    if (st == BookStatus::Ok && !writer.finish()) st = BookStatus::IoError;
    if (st != BookStatus::Ok) {
      LOG_ERR("FIB", "Chapter %u layout failed: %d (layout %u/%u refused %u, parse %u/%u refused %u B)", spineIndex,
              static_cast<int>(st), static_cast<unsigned>(scratch.highWater()), static_cast<unsigned>(layoutSize),
              static_cast<unsigned>(scratch.failedAllocSize()), static_cast<unsigned>(parseArena.highWater()),
              static_cast<unsigned>(parseSize), static_cast<unsigned>(parseArena.failedAllocSize()));
      layoutBuf.reset();
      parseBuf.reset();
      reallocChapterArenas();
      return st;
    }
    LOG_INF("FIB", "Chapter %u paginated: %u pages in %ums (layout %u/%u, parse %u/%u B, free heap %u)", spineIndex,
            writer.pageCount(), static_cast<unsigned>(millis() - t0), static_cast<unsigned>(scratch.highWater()),
            static_cast<unsigned>(layoutSize), static_cast<unsigned>(parseArena.highWater()),
            static_cast<unsigned>(parseSize), static_cast<unsigned>(ESP.getFreeHeap()));
  }
  layoutBuf.reset();
  parseBuf.reset();
  if (!reallocChapterArenas()) return BookStatus::OutOfMemory;

  indexArena_.reset();
  st = reader_.open(cache_, cacheName_, gen, indexArena_);
  if (st == BookStatus::Ok) curSpine_ = spineIndex;
  return st;
}

// Extracts (inflates) a deflated chapter to <cacheDir>/xNNNN.raw once, so the
// layout session parses a stored file (~8 KB resident parse state instead of
// the ~46 KB inflate stream). The file is keyed on the spine index only — it
// survives settings changes (generations) and is reused by rebuilds.
bool BookPaginator::extractChapter(const uint16_t spineIndex, const freeink::book::ZipEntry& entry) {
  char rawName[16];
  snprintf(rawName, sizeof(rawName), "x%04u.raw", spineIndex);
  char rawPath[128];
  snprintf(rawPath, sizeof(rawPath), "%s/%s", cacheDir_.c_str(), rawName);

  chapterSource_.close();
  if (!Storage.exists(rawPath) || (chapterSource_.open(rawPath) && chapterSource_.size() != entry.uncompressedSize)) {
    chapterSource_.close();
    // Inflate the whole entry to SD. Transient: one inflate stream (~46 KB).
    auto scratchBuf = makeUniqueNoThrow<uint8_t[]>(kOpenScratchSize);
    if (!scratchBuf) {
      LOG_ERR("FIB", "OOM: chapter extract scratch");
      return false;
    }
    Arena scratch(scratchBuf.get(), kOpenScratchSize);
    freeink::book::ZipEntryReader zr;
    if (zr.open(source_, entry, scratch) != BookStatus::Ok) return false;
    uint8_t* buf = static_cast<uint8_t*>(scratch.alloc(4096, 1));
    if (buf == nullptr) return false;
    char tmpPath[136];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", rawPath);
    {
      HalFile out;
      if (!Storage.openFileForWrite("FIB", tmpPath, out)) return false;
      for (;;) {
        const int32_t n = zr.read(buf, 4096);
        if (n < 0) {
          Storage.remove(tmpPath);
          return false;
        }
        if (n == 0) break;
        if (out.write(buf, n) != static_cast<size_t>(n)) {
          Storage.remove(tmpPath);
          return false;
        }
      }
    }
    Storage.remove(rawPath);
    if (!Storage.rename(tmpPath, rawPath)) return false;
    if (!chapterSource_.open(rawPath)) return false;
  }
  rawEntry_ = freeink::book::ZipEntryReader::rawEntry(entry.uncompressedSize);
  return true;
}

freeink::book::BookStatus BookPaginator::startIncremental(const uint16_t spineIndex,
                                                          const freeink::book::ZipEntry& entry,
                                                          const uint32_t targetChar, const float targetFraction,
                                                          const BuildProgress& progress) {
  // Resident while reading: layout (buffers + engine + writer index chunks —
  // a ~700-page chapter needs ~6 chunks) + a SMALL parse arena (the chapter
  // is stored/extracted, so no inflate window). The image pre-scan's probe
  // inflate (~46 KB) runs in a TRANSIENT arena freed right after begin() —
  // keeping it out of the resident set is what lets the session coexist
  // with the framebuffer and font caches on the C3. Expat pools stay on the
  // system heap (~19 KB while the session lives).
  constexpr size_t kBuildLayoutSize = 56 * 1024;
  constexpr size_t kBuildParseSize = 12 * 1024;
  constexpr size_t kPrescanSize = 46 * 1024;
  constexpr uint32_t kWindowPages = 2;  // build this far past the landing page

  // ensureChapter resolved curEntry_/chapterHref_ before calling here; the
  // `entry` argument aliases curEntry_ (a member -- the session's readers
  // retain pointers into it across steps).
  freeink::book::BookSource* chapterSrc = &source_;
  const freeink::book::ZipEntry* parseEntry = &entry;
  if (entry.method != 0) {
    if (!extractChapter(spineIndex, entry)) return BookStatus::IoError;
    chapterSrc = &chapterSource_;
    parseEntry = &rawEntry_;
  }

  buildLayoutBuf_ = makeUniqueNoThrow<uint8_t[]>(kBuildLayoutSize);
  buildParseBuf_ = makeUniqueNoThrow<uint8_t[]>(kBuildParseSize);
  if (!buildLayoutBuf_ || !buildParseBuf_) {
    LOG_ERR("FIB", "OOM: incremental build arenas (free heap %u)", static_cast<unsigned>(ESP.getFreeHeap()));
    buildLayoutBuf_.reset();
    buildParseBuf_.reset();
    return BookStatus::OutOfMemory;
  }
  buildLayoutArena_.init(buildLayoutBuf_.get(), kBuildLayoutSize);
  buildParseArena_.init(buildParseBuf_.get(), kBuildParseSize);

  buildWriter_ = freeink::book::PageCacheWriter();
  const uint32_t gen = generation();
  if (!buildWriter_.begin(cache_, cacheName_, gen, buildLayoutArena_)) {
    buildLayoutBuf_.reset();
    buildParseBuf_.reset();
    return BookStatus::IoError;
  }
  BookStatus st;
  {
    // Transient probe arena for begin() only (image dimension pre-scan).
    auto prescanBuf = makeUniqueNoThrow<uint8_t[]>(kPrescanSize);
    Arena prescanArena(prescanBuf.get(), prescanBuf ? kPrescanSize : 0);
    st = buildSession_.begin(source_, &zip(), *chapterSrc, *parseEntry, chapterHref_, params_, buildLayoutArena_,
                             buildWriter_, &buildParseArena_, prescanBuf ? &prescanArena : nullptr);
  }
  if (st != BookStatus::Ok) {
    LOG_ERR("FIB", "Incremental begin failed: %d", static_cast<int>(st));
    buildSession_.abort();
    buildWriter_ = freeink::book::PageCacheWriter();
    buildLayoutBuf_.reset();
    buildParseBuf_.reset();
    return st;
  }
  buildGeneration_ = gen;
  building_ = true;

  // Synchronous burst: lay out just past the landing position. For a char
  // target, "the target's page + window exists" is the stop (pageForChar is
  // a watermark). For a fraction target the stop is the parsed BYTE ratio —
  // the caller then resolves its landing char against chars-so-far, which by
  // construction sits right at this stop point.
  const uint32_t t0 = millis();
  while (!buildSession_.done()) {
    if (targetFraction >= 0.0f) {
      const uint64_t total = buildSession_.bytesTotal();
      if (total > 0 &&
          buildSession_.bytesConsumed() >= static_cast<uint64_t>(targetFraction * static_cast<float>(total)) &&
          buildWriter_.pageCount() > kWindowPages) {
        break;
      }
    } else if (buildWriter_.pageCount() > buildWriter_.pageForChar(targetChar) + kWindowPages) {
      break;
    }
    st = buildSession_.step(4);
    if (st != BookStatus::Ok) {
      LOG_ERR("FIB", "Incremental build failed: %d (layout %u/%u refused %u, parse %u/%u refused %u B)",
              static_cast<int>(st), static_cast<unsigned>(buildLayoutArena_.highWater()),
              static_cast<unsigned>(kBuildLayoutSize), static_cast<unsigned>(buildLayoutArena_.failedAllocSize()),
              static_cast<unsigned>(buildParseArena_.highWater()), static_cast<unsigned>(kBuildParseSize),
              static_cast<unsigned>(buildParseArena_.failedAllocSize()));
      suspendBuild();
      return st;
    }
    if (progress.fn != nullptr) progress.fn(progress.ctx, buildWriter_.pageCount());
  }
  if (buildSession_.done()) return finalizeBuild();
  LOG_INF("FIB", "Chapter %u building incrementally: %u pages to target in %ums (of ~%u est)", spineIndex,
          buildWriter_.pageCount(), static_cast<unsigned>(millis() - t0), static_cast<unsigned>(estimatedTotalPages()));
  return BookStatus::Ok;
}

freeink::book::BookStatus BookPaginator::pumpBuild(const uint32_t pages) {
  if (!building_) return BookStatus::Ok;
  const BookStatus st = buildSession_.step(pages);
  if (st != BookStatus::Ok) {
    LOG_ERR("FIB", "Incremental pump failed: %d", static_cast<int>(st));
    suspendBuild();
    return st;
  }
  if (buildSession_.done()) return finalizeBuild();
  return BookStatus::Ok;
}

freeink::book::BookStatus BookPaginator::finalizeBuild() {
  buildWriter_.setTotalChars(buildSession_.totalChars());
  const bool committed = buildWriter_.finish();
  const uint32_t pages = buildWriter_.pageCount();
  buildSession_.abort();
  buildWriter_ = freeink::book::PageCacheWriter();
  buildLayoutBuf_.reset();
  buildParseBuf_.reset();
  building_ = false;
  partialReaderOpen_ = false;

  reader_ = freeink::book::PageCacheReader();
  if (!indexBuf_ && !reallocChapterArenas()) return BookStatus::OutOfMemory;
  indexArena_.reset();
  if (!committed) return BookStatus::IoError;
  const BookStatus st = reader_.open(cache_, cacheName_, buildGeneration_, indexArena_);
  LOG_INF("FIB", "Incremental build finalized: %u pages (open %d, free heap %u)", pages, static_cast<int>(st),
          static_cast<unsigned>(ESP.getFreeHeap()));
  return st;
}

void BookPaginator::suspendBuild() {
  if (!building_) return;
  buildWriter_.setTotalChars(buildSession_.totalChars());  // chars-so-far watermark
  buildWriter_.suspend(static_cast<uint32_t>(buildSession_.bytesConsumed()),
                       static_cast<uint32_t>(buildSession_.bytesTotal()));
  buildSession_.abort();
  buildWriter_ = freeink::book::PageCacheWriter();
  buildLayoutBuf_.reset();
  buildParseBuf_.reset();
  building_ = false;
  partialReaderOpen_ = false;
  curSpine_ = kNoSpine;  // force a clean reopen (of the partial) next time
}

uint32_t BookPaginator::pageCount() const {
  if (!building_) return reader_.pageCount();
  const uint32_t w = buildWriter_.pageCount();
  const uint32_t r = partialReaderOpen_ ? reader_.pageCount() : 0;
  return w > r ? w : r;
}

uint32_t BookPaginator::totalChars() const {
  if (!building_) return reader_.totalChars();
  // A chars-so-far watermark: the denominator grows as the build advances,
  // exactly like the legacy incremental build. Display code uses
  // estimatedTotalPages() instead.
  const uint32_t w = buildSession_.totalChars();
  const uint32_t r = partialReaderOpen_ ? reader_.totalChars() : 0;
  return w > r ? w : r;
}

uint32_t BookPaginator::charStartOfPage(const uint32_t pageIndex) const {
  if (!building_) return reader_.charStart(pageIndex);
  if (pageIndex < buildWriter_.pageCount()) return buildWriter_.charStart(pageIndex);
  return partialReaderOpen_ ? reader_.charStart(pageIndex) : 0;
}

uint32_t BookPaginator::pageForChar(const uint32_t charOffset) const {
  if (!building_) return reader_.pageForChar(charOffset);
  // The rebuilt prefix is byte-identical to the partial's, so the indexes
  // agree wherever they overlap — just ask the source that has more pages.
  if (partialReaderOpen_ && reader_.pageCount() > buildWriter_.pageCount()) {
    return reader_.pageForChar(charOffset);
  }
  return buildWriter_.pageForChar(charOffset);
}

uint32_t BookPaginator::estimatedTotalPages() const {
  const uint32_t known = pageCount();
  uint64_t consumed = 0;
  uint64_t total = 0;
  uint64_t basePages = 0;
  if (building_) {
    consumed = buildSession_.bytesConsumed();
    total = buildSession_.bytesTotal();
    basePages = buildWriter_.pageCount();
  } else if (reader_.isPartial()) {
    consumed = reader_.buildBytesConsumed();
    total = reader_.buildBytesTotal();
    basePages = reader_.pageCount();
  } else {
    return known;
  }
  if (consumed == 0 || total == 0 || basePages == 0) return known;
  const uint64_t est = basePages * total / consumed;
  return est > known ? static_cast<uint32_t>(est) : known;
}

bool BookPaginator::charForAnchor(const char* fragment, uint32_t* charOut) const {
  if (fragment == nullptr || fragment[0] == '\0') return false;
  const uint32_t hash = ZipCatalog::hashPath(fragment);
  if (building_) {
    if (buildWriter_.charForAnchor(hash, charOut)) return true;
    return partialReaderOpen_ && reader_.charForAnchor(hash, charOut);
  }
  return reader_.charForAnchor(hash, charOut);
}

freeink::book::BookStatus BookPaginator::readPage(const uint32_t pageIndex, freeink::book::Page* out) {
  // pageBuf_ is released transiently around chapter builds; a null here means
  // a failed reallocation (the arena would point at freed memory).
  if (curSpine_ == kNoSpine || !pageBuf_) return BookStatus::NotFound;
  pageArena_.reset();
  if (building_) {
    if (pageIndex < buildWriter_.pageCount()) return buildWriter_.readPage(pageIndex, pageArena_, out);
    if (partialReaderOpen_ && pageIndex < reader_.pageCount()) {
      return reader_.readPage(pageIndex, pageArena_, out);
    }
    return BookStatus::NotFound;  // beyond the watermark — pump and retry
  }
  return reader_.readPage(pageIndex, pageArena_, out);
}

int BookPaginator::spineIndexForHref(const char* href) const {
  if (href == nullptr || href[0] == '\0' || isTxt_) return -1;
  if (catalogMode_) return catalog_.spineIndexForHref(href);
  for (size_t s = 0; s < book_.spineCount(); ++s) {
    const ManifestItem* item = book_.spineItem(s);
    if (item != nullptr && strcmp(item->href, href) == 0) return static_cast<int>(s);
  }
  return -1;
}

bool BookPaginator::spineZipEntry(const int spineIndex, freeink::book::ZipEntry* out) const {
  if (isTxt_ || spineIndex < 0) return false;
  if (catalogMode_) {
    return catalog_.spineEntry(static_cast<size_t>(spineIndex), out) == BookStatus::Ok;
  }
  const ManifestItem* item = book_.spineItem(static_cast<size_t>(spineIndex));
  const freeink::book::ZipEntry* e = item != nullptr ? book_.zip().find(item->href) : nullptr;
  if (e == nullptr) return false;
  *out = *e;
  return true;
}

BookPaginator::TocItem BookPaginator::tocItem(const size_t index) const {
  TocItem out{"", nullptr, -1, 0};
  if (isTxt_) return out;
  if (catalogMode_) {
    // Title/fragment are SD reads into the shared member buffers -- valid
    // until the next tocItem() call (see the TocItem declaration).
    freeink::book::BookCatalog::TocItem item;
    if (catalog_.tocItem(index, &item, tocTitleBuf_, sizeof(tocTitleBuf_), tocFragBuf_, sizeof(tocFragBuf_)) !=
        BookStatus::Ok) {
      return out;
    }
    out.title = tocTitleBuf_;
    out.fragment = item.hasFragment ? tocFragBuf_ : nullptr;
    out.spineIndex = item.spineIndex;
    out.depth = item.depth;
    return out;
  }
  const freeink::book::TocEntry* entry = book_.tocEntry(index);
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
  if (catalogMode_) return catalog_.tocIndexForSpine(spineIndex);
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

uint32_t BookPaginator::spineSizeAt(const size_t spineIndex) const {
  if (catalogMode_) return catalog_.spineSize(spineIndex);
  const ManifestItem* item = book_.spineItem(spineIndex);
  const freeink::book::ZipEntry* e = item != nullptr ? book_.zip().find(item->href) : nullptr;
  return e != nullptr ? e->uncompressedSize : 0;
}

// Spine weights use the uncompressed sizes already in the catalog -- the
// same "bigger chapters cover more of the book" heuristic the legacy engine
// used, with zero extra state.
float BookPaginator::bookProgress(const int spineIndex, const float chapterFraction) const {
  if (isTxt_) return chapterFraction;
  uint64_t before = 0;
  uint64_t current = 0;
  uint64_t total = 0;
  for (size_t s = 0; s < spineCount(); ++s) {
    const uint32_t size = spineSizeAt(s);
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
  if (isTxt_ || spineCount() == 0) {
    if (chapterFractionOut != nullptr) *chapterFractionOut = bookFraction;
    return 0;
  }
  uint64_t total = 0;
  for (size_t s = 0; s < spineCount(); ++s) {
    total += spineSizeAt(s);
  }
  const float f = bookFraction < 0.0f ? 0.0f : (bookFraction > 1.0f ? 1.0f : bookFraction);
  const uint64_t target = static_cast<uint64_t>(f * static_cast<float>(total));
  uint64_t cumulative = 0;
  for (size_t s = 0; s < spineCount(); ++s) {
    const uint32_t size = spineSizeAt(s);
    if (target < cumulative + size || s + 1 == spineCount()) {
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
