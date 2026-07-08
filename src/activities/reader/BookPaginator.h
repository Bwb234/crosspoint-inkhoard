#pragma once

// BookPaginator — one open book's FreeInkBook session: container, fonts,
// layout parameters, and the per-chapter page cache. This is the engine-side
// half of the reader; EpubReaderActivity keeps the UI half (input, menus,
// status bar, refresh cadence) and talks to the engine only through this.
//
// Memory model (ESP32-C3, no PSRAM; the reader sees only ~140 KB free, so
// nothing is sized for the worst case):
//   book arena    the SD-backed BookCatalog's resident tables, exact-sized
//                 from the catalog footer (a few KB for a normal book,
//                 ~42 KB for a 1,732-spine omnibus). Reopens skip the
//                 container parse entirely. Only when the one-time catalog
//                 build fails does open() fall back to the legacy in-RAM
//                 Book: probe-parse into a temporary arena to learn the real
//                 footprint, then re-open into precisely that many bytes
//   stylesheet    compacted to the actual rule array (typically < 2 KB)
//   index arena   12 KB  current chapter's page index + anchor table
//   page arena    12 KB  decoded runs of the page being rendered
//   open scratch  (transient 64 KB) container parse working set
//   build scratch (transient, ideally 120 KB, stepping down to 96 KB under
//                 fragmentation) only while a chapter (re)paginates; the
//                 index/page arenas are released around it to help the heap
//                 produce one contiguous block
// Steady-state page turns touch only the page arena (~2 KB used).

#include <BookCatalog.h>
#include <FreeInkBook.h>
#include <cache/PageCache.h>
#include <css/Css.h>
#include <layout/ChapterLayout.h>
#include <render/TtfFont.h>
#include <text/Hyphenator.h>

#include <memory>
#include <string>

#include "CpFontAdapter.h"
#include "FreeInkBookStorage.h"

class GfxRenderer;

class BookPaginator {
 public:
  // Called periodically during a chapter (re)pagination so the UI can show
  // an indexing popup / keep the watchdog fed.
  struct BuildProgress {
    void* ctx;
    void (*fn)(void* ctx, uint32_t pagesDone);
  };  // value-initialized ({nullptr, nullptr}) by the default argument

  BookPaginator() = default;
  ~BookPaginator() { close(); }
  BookPaginator(const BookPaginator&) = delete;
  BookPaginator& operator=(const BookPaginator&) = delete;

  // Opens the container and builds the font chain. `cacheDir` is the
  // per-book directory (".crosspoint/epub_<hash>"). Plain-text files open
  // as a one-chapter book with no container — detected by .txt extension, or
  // forced via `forcePlainText` (markdown files read as plain text).
  bool open(const std::string& path, const std::string& cacheDir, GfxRenderer& renderer, bool forcePlainText = false);
  void close();
  bool isOpen() const { return open_; }
  bool isTxt() const { return isTxt_; }

  freeink::book::BookSource* bookSource() { return &source_; }
  // Container lookups (image probes, internal links) -- the in-RAM catalog or
  // the SD-backed one, whichever this book opened with.
  const freeink::book::ZipCatalog& zip() const { return catalogMode_ ? catalog_.zip() : book_.zip(); }
  // True when the book runs on the SD-backed BookCatalog -- the default for
  // every EPUB; false only after a catalog build failure forced the legacy
  // in-RAM fallback.
  bool isCatalogMode() const { return catalogMode_; }
  size_t spineCount() const {
    if (isTxt_) return 1;
    return catalogMode_ ? catalog_.spineCount() : book_.spineCount();
  }
  const char* language() const;
  const char* title() const {
    return isTxt_ ? "" : (catalogMode_ ? catalog_.metadata().title : book_.metadata().title);
  }
  const char* author() const {
    return isTxt_ ? "" : (catalogMode_ ? catalog_.metadata().author : book_.metadata().author);
  }
  // ZipEntry of a spine item (KOSync/xpath bridges). False when absent.
  bool spineZipEntry(int spineIndex, freeink::book::ZipEntry* out) const;

  // --- TOC (flattened, resolved to spine indices) --------------------------
  struct TocItem {
    // In catalog mode both strings live in a shared internal buffer that the
    // NEXT tocItem() call overwrites -- use or copy them before iterating on.
    const char* title;
    const char* fragment;  // anchor within the chapter, or nullptr
    int spineIndex;        // -1 when the href is not a spine item
    uint8_t depth;
  };
  size_t tocCount() const {
    if (isTxt_) return 0;
    return catalogMode_ ? catalog_.tocCount() : book_.tocCount();
  }
  TocItem tocItem(size_t index) const;
  // First TOC entry pointing at `spineIndex` or an earlier chapter (the
  // chapter's display title); -1 when the TOC has no such entry.
  int tocIndexForSpine(int spineIndex) const;

  // --- whole-book progress (uncompressed spine byte weights) ---------------
  // Fraction of the book at (spine, fraction-within-chapter), 0..1.
  float bookProgress(int spineIndex, float chapterFraction) const;
  // Inverse: which spine (and where inside it) a whole-book fraction lands.
  int spineForBookFraction(float bookFraction, float* chapterFractionOut) const;

  // Refreshes LayoutParams from SETTINGS and the given content box. Must be
  // called before ensureChapter() and after any settings/orientation change;
  // a changed generation makes the next ensureChapter() re-paginate.
  void configureLayout(int16_t pageWidth, int16_t pageHeight, int16_t marginLeft, int16_t marginRight,
                       int16_t marginTop, int16_t marginBottom);

  // Everything layout-relevant, hashed — the cache key.
  uint32_t generation() const;

  // True when the chapter already has a page cache for the current
  // generation — i.e. ensureChapter() will be a fast open, not a build.
  // Callers use this to show the indexing popup before the heavy path.
  bool isChapterCached(uint16_t spineIndex);

  // Opens the chapter's page cache for the current generation, laying the
  // chapter out first when missing or stale. Heavy only on that miss.
  //
  // `targetChar` — the landing position (chapter character offset), when the
  // caller knows it. For a GIANT uncached chapter (or a suspended partial)
  // this switches to the INCREMENTAL path: the chapter builds only a little
  // past the target, isBuilding() turns true, pages serve from the partial
  // build, and the caller finishes the rest via pumpBuild() between page
  // turns. kBuildAll (the default) forces the classic blocking full build —
  // required for percent jumps (they need the final page count).
  // `targetFraction` (0..1) is the fraction-of-chapter alternative for
  // landings that predate exact charStart positions (migrated legacy
  // progress, whole-book percent jumps): the incremental build runs until
  // the parsed BYTE ratio passes it — bytes track extracted characters
  // closely enough that the subsequent pageForChar lands within a page.
  static constexpr uint32_t kBuildAll = 0xFFFFFFFF;
  freeink::book::BookStatus ensureChapter(uint16_t spineIndex, const BuildProgress& progress = BuildProgress(),
                                          uint32_t targetChar = kBuildAll, float targetFraction = -1.0f);
  bool chapterReady() const { return curSpine_ != kNoSpine; }
  uint16_t currentSpine() const { return curSpine_; }

  // --- incremental build (giant single-spine chapters) ---------------------
  bool isBuilding() const { return building_; }
  // Lays out up to `pages` more pages of the in-progress build; finalizes
  // (writer commit + cache reopen) when the chapter ends. Callers gate this
  // on a read-ahead window (currentPage + N) — never pump unboundedly.
  freeink::book::BookStatus pumpBuild(uint32_t pages);
  // Byte-ratio estimate of the final page count while building ("page X of
  // ~Y"); the exact count once the build (or cache) is complete.
  uint32_t estimatedTotalPages() const;

  // While building incrementally these merge the live writer (rebuild
  // watermark) with any reopened partial — see the .cpp implementations.
  uint32_t pageCount() const;
  uint32_t totalChars() const;
  uint32_t charStartOfPage(uint32_t pageIndex) const;
  uint32_t pageForChar(uint32_t charOffset) const;
  // Resolve an id="" fragment in the CURRENT chapter to a char offset.
  bool charForAnchor(const char* fragment, uint32_t* charOut) const;

  // Decodes one page into the page arena; the returned Page's records stay
  // valid until the next readPage() call.
  freeink::book::BookStatus readPage(uint32_t pageIndex, freeink::book::Page* out);

  // Spine index for a container href (link/TOC targets), -1 when absent.
  int spineIndexForHref(const char* href) const;

  // Render-path lockstep: the renderer font id for a run's sizePx, quantized
  // exactly as CpFontAdapter quantized during layout.
  int fontIdForRunSize(uint16_t sizePx) const;
  // The ladder rung (px) a run size resolves to — for underline metrics etc.
  uint16_t quantizeRunSize(uint16_t sizePx) const { return adapters_[0].quantize(sizePx); }

  const freeink::book::LayoutParams& layoutParams() const { return params_; }

 private:
  static constexpr uint16_t kNoSpine = 0xFFFF;
  static constexpr size_t kBookArenaSize = 48 * 1024;
  static constexpr size_t kIndexArenaSize = 12 * 1024;
  static constexpr size_t kPageArenaSize = 12 * 1024;
  static constexpr size_t kSheetArenaSize = 12 * 1024;
  // Container open: one inflate stream + package parse (~47 KB measured
  // high-water). Deliberately snug: open needs scratch + the 48 KB temp book
  // arena simultaneously, and the C3's largest free block hovers just above
  // 112 KB — 64 KB here made the pair miss fitting by a few dozen bytes.
  // (Chapter builds size their two arenas adaptively in ensureChapter.)
  static constexpr size_t kOpenScratchSize = 56 * 1024;

  bool buildFontChain(GfxRenderer& renderer);
  bool reallocChapterArenas();
  void loadHyphenator();
  uint32_t fontFingerprint() const;
  // Uncompressed bytes of a spine item -- the whole-book progress weights.
  uint32_t spineSizeAt(size_t spineIndex) const;
  // SD-backed catalog path (the default open path for every EPUB): open an
  // existing catalog.fibc / build one. openCatalog removes a stale index
  // (changed container) and returns false so the caller can rebuild;
  // buildCatalog expects the framebuffer already lent.
  bool openCatalog();
  bool buildCatalog();
  // Builds + compacts the book stylesheet (CSS items from whichever catalog).
  void buildStylesheet(freeink::book::Arena& scratch);

  // Incremental-build internals. A "giant" chapter (uncompressed size above
  // kIncrementalThreshold) is extracted once to <cacheDir>/xNNNN.raw so the
  // resident parse state is the stored-entry ~8 KB plus probe headroom, then
  // laid out through a ChapterLayoutSession feeding buildWriter_. Session
  // arenas (buildLayoutBuf_/buildParseBuf_) stay allocated while reading;
  // suspendBuild() commits a partial cache on exit/spine-switch.
  freeink::book::BookStatus startIncremental(uint16_t spineIndex, const freeink::book::ZipEntry& entry,
                                             uint32_t targetChar, float targetFraction, const BuildProgress& progress);
  freeink::book::BookStatus finalizeBuild();
  void suspendBuild();
  bool extractChapter(uint16_t spineIndex, const freeink::book::ZipEntry& entry);

  SdBookSource source_;
  SdCacheStorage cache_;
  freeink::book::Book book_;            // legacy in-RAM fallback (catalog build failed)
  freeink::book::BookCatalog catalog_;  // SD-backed container index (default)
  freeink::book::LayoutParams params_;
  freeink::book::PageCacheReader reader_;
  freeink::book::FontChain chain_;
  CpFontAdapter adapters_[4] = {
      CpFontAdapter(EpdFontFamily::REGULAR),
      CpFontAdapter(EpdFontFamily::BOLD),
      CpFontAdapter(EpdFontFamily::ITALIC),
      CpFontAdapter(EpdFontFamily::BOLD_ITALIC),
  };
  freeink::book::Hyphenator hyphenator_;
  std::unique_ptr<uint8_t[]> hyphBlob_;  // SD-loaded patterns (non-English books)

  std::unique_ptr<uint8_t[]> bookBuf_;   // exact-sized book arena backing
  std::unique_ptr<uint8_t[]> indexBuf_;  // released transiently around builds
  std::unique_ptr<uint8_t[]> pageBuf_;   // released transiently around builds
  std::unique_ptr<uint8_t[]> sheetBuf_;  // compacted CssRule array
  freeink::book::Arena bookArena_;
  freeink::book::Arena indexArena_;
  freeink::book::Arena pageArena_;
  freeink::book::CssStylesheet sheet_{};

  // PageCacheReader borrows this for readPage() — must outlive the reader.
  char cacheName_[64] = "";
  int ladderFontIds_[CpFontAdapter::kMaxLadder] = {};
  uint16_t ladderSizes_[CpFontAdapter::kMaxLadder] = {};
  uint8_t ladderCount_ = 0;
  uint16_t curSpine_ = kNoSpine;
  bool open_ = false;
  bool isTxt_ = false;
  bool catalogMode_ = false;
  std::string cacheDir_;  // per-book cache directory (extraction files live here)

  // Current chapter identity. curEntry_ must be a MEMBER: the incremental
  // layout session's ZipEntryReader retains a pointer to it across steps, and
  // in catalog mode there is no arena-resident entry to point at.
  freeink::book::ZipEntry curEntry_{};
  char chapterHref_[512] = "";
  // tocItem() string backing in catalog mode (single slot, see TocItem note).
  mutable char tocTitleBuf_[256];
  mutable char tocFragBuf_[256];

  // Incremental-build session state (live only while building_).
  freeink::book::ChapterLayoutSession buildSession_;
  freeink::book::PageCacheWriter buildWriter_;
  SdBookSource chapterSource_;                 // the extracted raw chapter file
  freeink::book::ZipEntry rawEntry_{};         // headerless entry over chapterSource_
  std::unique_ptr<uint8_t[]> buildLayoutBuf_;  // session layout arena backing
  std::unique_ptr<uint8_t[]> buildParseBuf_;   // session parse arena backing
  freeink::book::Arena buildLayoutArena_;
  freeink::book::Arena buildParseArena_;
  uint32_t buildGeneration_ = 0;  // generation the session was started under
  bool building_ = false;
  bool partialReaderOpen_ = false;  // reader_ holds a partial while rebuilding
};
