#pragma once

// BookPaginator — one open book's FreeInkBook session: container, fonts,
// layout parameters, and the per-chapter page cache. This is the engine-side
// half of the reader; EpubReaderActivity keeps the UI half (input, menus,
// status bar, refresh cadence) and talks to the engine only through this.
//
// Memory model (ESP32-C3, no PSRAM — all buffers heap-allocated once in
// open() and freed in close()):
//   book arena   64 KB  ZIP catalog + metadata + spine + TOC (corpus
//                       high-water 5-40 KB; omnibus containers that exceed
//                       it fail with a clean OutOfMemory)
//   index arena  16 KB  current chapter's page index + anchor table
//   page arena   16 KB  decoded runs of the page being rendered
//   build scratch (transient, ~120 KB) allocated only while a chapter
//                       (re)paginates — the layout engine's whole working set
// Steady-state page turns touch only the page arena (~2 KB used).

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

  freeink::book::Book& book() { return book_; }
  freeink::book::BookSource* bookSource() { return &source_; }
  size_t spineCount() const { return isTxt_ ? 1 : book_.spineCount(); }
  const char* language() const;
  const char* title() const { return isTxt_ ? "" : book_.metadata().title; }
  const char* author() const { return isTxt_ ? "" : book_.metadata().author; }

  // --- TOC (flattened, resolved to spine indices) --------------------------
  struct TocItem {
    const char* title;
    const char* fragment;  // anchor within the chapter, or nullptr
    int spineIndex;        // -1 when the href is not a spine item
    uint8_t depth;
  };
  size_t tocCount() const { return isTxt_ ? 0 : book_.tocCount(); }
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

  // Opens the chapter's page cache for the current generation, laying the
  // chapter out first when missing or stale. Heavy only on that miss.
  freeink::book::BookStatus ensureChapter(uint16_t spineIndex, const BuildProgress& progress = BuildProgress());
  bool chapterReady() const { return curSpine_ != kNoSpine; }
  uint16_t currentSpine() const { return curSpine_; }

  uint32_t pageCount() const { return reader_.pageCount(); }
  uint32_t totalChars() const { return reader_.totalChars(); }
  uint32_t charStartOfPage(uint32_t pageIndex) const { return reader_.charStart(pageIndex); }
  uint32_t pageForChar(uint32_t charOffset) const { return reader_.pageForChar(charOffset); }
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
  static constexpr size_t kBuildScratchSize = 120 * 1024;

  bool buildFontChain(GfxRenderer& renderer);
  void loadHyphenator();
  uint32_t fontFingerprint() const;

  SdBookSource source_;
  SdCacheStorage cache_;
  freeink::book::Book book_;
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

  std::unique_ptr<uint8_t[]> bookBuf_;
  std::unique_ptr<uint8_t[]> indexBuf_;
  std::unique_ptr<uint8_t[]> pageBuf_;
  std::unique_ptr<uint8_t[]> sheetBuf_;
  freeink::book::Arena bookArena_;
  freeink::book::Arena indexArena_;
  freeink::book::Arena pageArena_;
  freeink::book::Arena sheetArena_;
  freeink::book::CssStylesheet sheet_{};

  // PageCacheReader borrows this for readPage() — must outlive the reader.
  char cacheName_[64] = "";
  int ladderFontIds_[CpFontAdapter::kMaxLadder] = {};
  uint16_t ladderSizes_[CpFontAdapter::kMaxLadder] = {};
  uint8_t ladderCount_ = 0;
  uint16_t curSpine_ = kNoSpine;
  bool open_ = false;
  bool isTxt_ = false;
};
